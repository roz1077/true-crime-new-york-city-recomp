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
$compiler = Join-Path $BuildDir 'compiler'
if (!(Test-Path -LiteralPath $compiler -PathType Container)) {
    throw "Compiler directory not found: $compiler. Build the release target first."
}
$sdkLicence = Join-Path $SdkDir 'LICENSE'
if (!(Test-Path -LiteralPath $sdkLicence -PathType Leaf)) {
    throw "SDK licence not found: $sdkLicence"
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
