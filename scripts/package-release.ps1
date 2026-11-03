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

function Add-ReleaseFile($File, $Entry) {
    $source = (Resolve-Path -LiteralPath $File).Path
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $source, $Entry) | Out-Null
}

try {
    $zip = [IO.Compression.ZipFile]::Open($temp, 'Create')
    try {
        $zip.CreateEntry('assets/') | Out-Null
        Add-ReleaseFile (Join-Path $BuildDir 'TrueCrimeNYC.exe') 'TrueCrimeNYC.exe'
        Add-ReleaseFile (Join-Path $root 'README.md') 'README.md'
        Add-ReleaseFile (Join-Path $SdkDir 'THIRD_PARTY.txt') 'THIRD_PARTY.txt'

        $compiler = (Resolve-Path -LiteralPath (Join-Path $BuildDir 'compiler')).Path
        foreach ($name in 'clang.exe', 'lld-link.exe', 'xbe_recomp.exe', 'TrueCrimeNYC.lib', 'LLVM-LICENSE.txt') {
            Add-ReleaseFile (Join-Path $compiler $name) "compiler/$name"
        }
        foreach ($directory in 'config', 'include', 'xdk') {
            Get-ChildItem -LiteralPath (Join-Path $compiler $directory) -Recurse -File | ForEach-Object {
                $relative = $_.FullName.Substring($compiler.Length + 1).Replace('\', '/')
                Add-ReleaseFile $_.FullName "compiler/$relative"
            }
        }
        foreach ($name in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
            $runtime = Join-Path $RuntimeDir $name
            Add-ReleaseFile $runtime $name
            Add-ReleaseFile $runtime "compiler/$name"
        }
    } finally {
        $zip.Dispose()
    }
    Move-Item -LiteralPath $temp -Destination $output -Force
    Write-Output "Created $output"
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
}
