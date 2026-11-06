$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $project
try {
    & mingw32-make.exe -j4 stage
    if ($LASTEXITCODE -ne 0) { throw 'ARM build failed; do not copy old SD files.' }
    & (Join-Path $PSScriptRoot 'verify-build.ps1')
    & (Join-Path $PSScriptRoot 'update-sd-manifest.ps1')
    & (Join-Path $PSScriptRoot 'verify-sdcard.ps1')
    Write-Output 'Ready: copy the contents of sdcard to the FAT32 root of your microSD.'
} finally { Pop-Location }
