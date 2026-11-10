param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$SdkDir,
    [Parameter(Mandatory = $true)][string]$RuntimeDir
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = Split-Path -Parent $PSScriptRoot
$output = Join-Path $root 'TrueCrimeNYC.zip'
$temp = Join-Path $root ('TrueCrimeNYC-' + [guid]::NewGuid() + '.tmp')

# Fixed archive timestamps make the zip reproducible. SOURCE_DATE_EPOCH (a Unix
# timestamp) overrides the default, which is 2000-01-01T00:00:00Z.
$epoch = 946684800L
if ($env:SOURCE_DATE_EPOCH) {
    try {
        $epoch = [long]$env:SOURCE_DATE_EPOCH
    } catch {
        throw "SOURCE_DATE_EPOCH must be an integer Unix timestamp, not '$env:SOURCE_DATE_EPOCH'."
    }
}
if ($epoch -lt 315532800) {
    throw 'SOURCE_DATE_EPOCH must be 315532800 (1980-01-01) or later; the ZIP format cannot store older dates.'
}
$entryTime = [DateTimeOffset]::FromUnixTimeSeconds($epoch).ToUniversalTime()
$hashLines = New-Object 'System.Collections.Generic.List[string]'

function Add-ReleaseDirectory([string]$Entry) {
    $zipEntry = $zip.CreateEntry($Entry)
    $zipEntry.LastWriteTime = $script:entryTime
}

function Add-ReleaseFile([string]$File, [string]$Entry) {
    if (!(Test-Path -LiteralPath $File -PathType Leaf)) {
        throw "Missing release file: $File"
    }
    $source = (Resolve-Path -LiteralPath $File).Path
    $zipEntry = $zip.CreateEntry($Entry, [IO.Compression.CompressionLevel]::Optimal)
    $zipEntry.LastWriteTime = $script:entryTime
    $input = [IO.File]::OpenRead($source)
    try {
        $output = $zipEntry.Open()
        try {
            $input.CopyTo($output)
        } finally {
            $output.Dispose()
        }
    } finally {
        $input.Dispose()
    }
    $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    $script:hashLines.Add("$hash  $Entry")
}

$exe = Join-Path $BuildDir 'TrueCrimeNYC.exe'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Release executable not found: $exe. Build the release target first."
}
# Developer debug tools must never ship (docs/debug-menu-plan.md): refuse a build directory
# configured with them, and an executable that contains the dev build marker.
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if ((Test-Path -LiteralPath $cache -PathType Leaf) -and
    (Select-String -LiteralPath $cache -Pattern '^TCNYC_DEV_TOOLS:BOOL=(ON|TRUE|1|YES)$' -Quiet)) {
    throw "$BuildDir was configured with TCNYC_DEV_TOOLS=ON. Reconfigure it with -DTCNYC_DEV_TOOLS=OFF (or use the release preset in a clean directory) before packaging."
}
# Only an optimised build ships: Debug and RelWithDebInfo are slower and carry debug data.
if (Test-Path -LiteralPath $cache -PathType Leaf) {
    $type = Select-String -LiteralPath $cache -Pattern '^CMAKE_BUILD_TYPE:STRING=(.*)$' | Select-Object -First 1
    if ($type -and $type.Matches[0].Groups[1].Value -ne 'Release') {
        throw "$BuildDir is a $($type.Matches[0].Groups[1].Value) build. Package a Release build (the release preset)."
    }
}
$marker = [Text.Encoding]::ASCII.GetBytes('TCNYC-DEV-TOOLS-BUILD')
$exeBytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $exe).Path)
$exeText = [Text.Encoding]::GetEncoding(28591).GetString($exeBytes)
if ($exeText.Contains([Text.Encoding]::GetEncoding(28591).GetString($marker))) {
    throw "$exe contains the developer debug tools (TCNYC-DEV-TOOLS-BUILD). Rebuild with TCNYC_DEV_TOOLS=OFF before packaging."
}

$compiler = Join-Path $BuildDir 'compiler'
if (!(Test-Path -LiteralPath $compiler -PathType Container)) {
    throw "Compiler directory not found: $compiler. Build the release target first."
}
$sdkLicence = Join-Path $SdkDir 'LICENSE'
if (!(Test-Path -LiteralPath $sdkLicence -PathType Leaf)) {
    throw "SDK licence not found: $sdkLicence"
}

# The package must run on a plain Windows install. A build made with a MinGW-target clang (for
# example llvm-mingw from winget, when it is first on PATH) links the game against libc++.dll and
# libunwind.dll, and bundles a clang.exe launcher that needs clang-<version>.exe and libLLVM beside
# it. Refuse both: every DLL the executables import must be a Windows system DLL or one this
# package ships, and the bundled compiler must start from a folder of its own.
function Get-PeImports([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    $sections = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $optional = $pe + 24
    $optionalSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
    $dirs = if ([BitConverter]::ToUInt16($bytes, $optional) -eq 0x20B) { $optional + 112 } else { $optional + 96 }
    $importRva = [BitConverter]::ToUInt32($bytes, $dirs + 8)
    $table = $optional + $optionalSize
    $toOffset = {
        param([uint32]$rva)
        for ($i = 0; $i -lt $sections; $i++) {
            $s = $table + $i * 40
            $va = [BitConverter]::ToUInt32($bytes, $s + 12)
            $size = [Math]::Max([BitConverter]::ToUInt32($bytes, $s + 8), [BitConverter]::ToUInt32($bytes, $s + 16))
            if ($rva -ge $va -and $rva -lt $va + $size) { return $rva - $va + [BitConverter]::ToUInt32($bytes, $s + 20) }
        }
        throw "${Path}: import RVA $rva is outside every section"
    }
    $names = @()
    if (!$importRva) { return $names }
    for ($d = & $toOffset $importRva; [BitConverter]::ToUInt32($bytes, $d + 12); $d += 20) {
        $n = & $toOffset ([BitConverter]::ToUInt32($bytes, $d + 12))
        $end = [Array]::IndexOf($bytes, [byte]0, [int]$n)
        $names += [Text.Encoding]::ASCII.GetString($bytes, $n, $end - $n)
    }
    return $names
}
$system32 = Join-Path $env:SystemRoot 'System32'
$shipped = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
# The Windows DLLs the game and its bundled tools may import. Any other DLL, even one that happens
# to be in this machine's System32 (a driver's or another program's), would be missing elsewhere.
$windows = @('kernel32.dll', 'ntdll.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll',
    'oleaut32.dll', 'comdlg32.dll', 'imm32.dll', 'setupapi.dll', 'version.dll', 'winmm.dll', 'ws2_32.dll',
    'dwmapi.dll', 'dbghelp.dll', 'windowscodecs.dll', 'd3d12.dll', 'dxgi.dll', 'd3dcompiler_47.dll',
    'shlwapi.dll', 'uxtheme.dll', 'hid.dll', 'cfgmgr32.dll', 'bcrypt.dll', 'crypt32.dll', 'psapi.dll',
    'userenv.dll', 'rpcrt4.dll', 'xinput1_4.dll', 'iphlpapi.dll', 'secur32.dll', 'combase.dll')
$executables = @($exe) + @('clang.exe', 'lld-link.exe', 'xbe_recomp.exe' | ForEach-Object { Join-Path $compiler $_ })
foreach ($binary in $executables) {
    foreach ($dll in Get-PeImports $binary) {
        $name = $dll.ToLowerInvariant()
        if ($name -like 'api-ms-win-*' -or $name -like 'ext-ms-*' -or $shipped -contains $name) { continue }
        if ($windows -notcontains $name -or !(Test-Path -LiteralPath (Join-Path $system32 $dll) -PathType Leaf)) {
            throw "$binary needs $dll, which is neither a Windows DLL nor in the package. Build the release with the MSVC-target clang (build.bat puts Visual Studio's or C:\Program Files\LLVM's first on PATH), not a MinGW toolchain."
        }
    }
}
$isolated = Join-Path ([IO.Path]::GetTempPath()) ('tcnyc-compiler-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $isolated | Out-Null
try {
    foreach ($name in 'clang.exe', 'lld-link.exe', 'xbe_recomp.exe') { Copy-Item -LiteralPath (Join-Path $compiler $name) -Destination $isolated }
    foreach ($name in $shipped) { Copy-Item -LiteralPath (Join-Path $RuntimeDir $name) -Destination $isolated }
    $savedPath = $env:PATH
    $env:PATH = "$isolated;$system32;$env:SystemRoot"
    try {
        foreach ($name in 'clang.exe', 'lld-link.exe') {
            $tool = Join-Path $isolated $name
            $out = & $tool --version 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0 -or $out -notmatch 'LLVM|clang') {
                throw "The bundled compiler/$name does not run on its own (exit $LASTEXITCODE): $($out.Trim()). Build the release with the MSVC-target clang, not a MinGW toolchain."
            }
        }
        # xbe_recomp has no --version: started with a missing input it reports that and exits. Only
        # a loader failure (0xC0000135, a DLL not found) means it cannot run from the package.
        $out = & (Join-Path $isolated 'xbe_recomp.exe') (Join-Path $isolated 'missing.xbe') 2>&1 | Out-String
        if ($LASTEXITCODE -eq -1073741515 -or !$out.Trim()) {
            throw "The bundled compiler/xbe_recomp.exe does not start on its own (exit $LASTEXITCODE). Rebuild it with the MSVC-target clang."
        }
    } finally {
        $env:PATH = $savedPath
    }
} finally {
    Remove-Item -LiteralPath $isolated -Recurse -Force -ErrorAction SilentlyContinue
}

# Compute provenance before the temporary archive exists in the repository, so
# packaging itself cannot make the tree look dirty.
$version = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
if (!$version) { $version = 'unknown' }
try { $git = (git -C $root rev-parse --short HEAD 2>$null) } catch { $git = $null }
if (!$git) { $git = 'unknown' }
else {
    $dirty = git -C $root status --porcelain 2>$null
    if ($dirty) { $git = "$git-dirty" }
}

try {
    $zip = [IO.Compression.ZipFile]::Open($temp, 'Create')
    try {
        Add-ReleaseDirectory 'assets/'
        Add-ReleaseDirectory 'LICENSES/'
        Add-ReleaseFile $exe 'TrueCrimeNYC.exe'
        Add-ReleaseFile (Join-Path $root 'README.md') 'README.md'
        Add-ReleaseFile (Join-Path $root 'README-LINUX.md') 'README-LINUX.md'
        Add-ReleaseFile (Join-Path $root 'CHANGELOG.md') 'CHANGELOG.md'
        Add-ReleaseFile (Join-Path $SdkDir 'THIRD_PARTY.txt') 'THIRD_PARTY.txt'
        Add-ReleaseFile $sdkLicence 'LICENSES/XboxXBE-Recomp-LICENSE.txt'
        Add-ReleaseFile (Join-Path $root 'LICENSE') 'LICENSES/Project-LICENSE.txt'

        $compiler = (Resolve-Path -LiteralPath $compiler).Path
        foreach ($name in 'clang.exe', 'lld-link.exe', 'xbe_recomp.exe', 'TrueCrimeNYC.lib', 'LLVM-LICENSE.txt') {
            Add-ReleaseFile (Join-Path $compiler $name) "compiler/$name"
        }
        foreach ($directory in 'config', 'include', 'xdk') {
            $path = Join-Path $compiler $directory
            if (!(Test-Path -LiteralPath $path -PathType Container)) {
                throw "Missing compiler directory: $path"
            }
            Get-ChildItem -LiteralPath $path -Recurse -File | Sort-Object -Property FullName | ForEach-Object {
                $relative = $_.FullName.Substring($compiler.Length + 1).Replace('\', '/')
                Add-ReleaseFile $_.FullName "compiler/$relative"
            }
        }
        foreach ($name in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
            $runtime = Join-Path $RuntimeDir $name
            Add-ReleaseFile $runtime $name
            Add-ReleaseFile $runtime "compiler/$name"
        }

        $buildText = "True Crime: New York City - Windows build`nVersion: $version`nGit: $git`nBuilt: $($entryTime.ToString('yyyy-MM-ddTHH:mm:ssZ'))`n"
        $buildEntry = $zip.CreateEntry('BUILD.txt')
        $buildEntry.LastWriteTime = $entryTime
        $buildEncoding = New-Object System.Text.UTF8Encoding($false)
        $buildWriter = New-Object System.IO.StreamWriter($buildEntry.Open(), $buildEncoding)
        try { $buildWriter.Write($buildText) } finally { $buildWriter.Dispose() }
        $buildBytes = $buildEncoding.GetBytes($buildText)
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $buildHash = -join ($sha.ComputeHash($buildBytes) | ForEach-Object { $_.ToString('x2') }) }
        finally { $sha.Dispose() }
        $hashLines.Add("$buildHash  BUILD.txt")

        # Checksums of the files above, stored inside the archive. The checksum
        # file itself is not listed.
        $sumEntry = $zip.CreateEntry('SHA256SUMS')
        $sumEntry.LastWriteTime = $entryTime
        $encoding = New-Object System.Text.UTF8Encoding($false)
        $writer = New-Object System.IO.StreamWriter($sumEntry.Open(), $encoding)
        try {
            foreach ($line in $hashLines) {
                $writer.Write($line + "`n")
            }
        } finally {
            $writer.Dispose()
        }
    } finally {
        $zip.Dispose()
    }
    Move-Item -LiteralPath $temp -Destination $output -Force
    $zipHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$output.sha256" -Value "$zipHash  TrueCrimeNYC.zip" -Encoding Ascii
    Write-Output "Created $output"
    Write-Output "SHA256 $zipHash"
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
}
