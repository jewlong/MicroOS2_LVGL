$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$project = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -Raw -LiteralPath (Join-Path $project 'firmware-lock.json') | ConvertFrom-Json
if ($lock.commit -notmatch '^[0-9a-f]{40}$') { throw 'Invalid firmware commit.' }
$sdRoot = Join-Path $project 'sdcard'
New-Item -ItemType Directory -Force -Path $sdRoot | Out-Null

# Check every download before replacing any staged file.
foreach ($file in $lock.files) {
    if ($file.name -notmatch '^[A-Za-z0-9_.-]+$' -or $file.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'Invalid firmware file entry.'
    }
    $temporary = Join-Path $sdRoot ($file.name + '.download')
    $uri = 'https://raw.githubusercontent.com/raspberrypi/firmware/' + $lock.commit + '/boot/' + $file.name
    Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $temporary
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash
    if ($actual -ne $file.sha256) { throw "Firmware hash mismatch: $($file.name)" }
}
foreach ($file in $lock.files) {
    Move-Item -Force -LiteralPath (Join-Path $sdRoot ($file.name + '.download')) -Destination (Join-Path $sdRoot $file.name)
    Write-Output "Verified: $($file.name)"
}
Write-Output 'Next: run make stage, update-sd-manifest.ps1, then verify-sdcard.ps1.'
