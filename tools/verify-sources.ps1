# Validate the pinned vendor sources and Raspberry Pi firmware.
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -Raw -LiteralPath (Join-Path $project 'ucos2-lock.json') | ConvertFrom-Json
$vendorRoot = Join-Path $project 'vendor/ucos2'
$count = 0
foreach ($file in $lock.files) {
    if ($file.path -match '\.\.' -or $file.path -notmatch '^[A-Za-z0-9_./-]+$') { throw 'Invalid source path' }
    $path = Join-Path $vendorRoot $file.path
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
    if ($actual -ne $file.sha256) { throw "Vendor mismatch: $($file.path)" }
    ++$count
}
Write-Output "PASS: $count unmodified vendor files at $($lock.commit)"
$firmware = Get-Content -Raw -LiteralPath (Join-Path $project 'firmware-lock.json') | ConvertFrom-Json
foreach ($file in $firmware.files) {
    $path = Join-Path (Join-Path $project 'sdcard') $file.name
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $file.sha256) {
        throw "Firmware mismatch: $($file.name)"
    }
}
Write-Output "PASS: firmware files match fixed commit $($firmware.commit)"

$lvgl = Get-Content -Raw -LiteralPath (Join-Path $project 'lvgl-lock.json') | ConvertFrom-Json
$lvglRoot = Join-Path $project 'vendor/lvgl'
foreach ($file in $lvgl.files) {
    if ($file.path -match '\.\.' -or $file.path -notmatch '^[A-Za-z0-9_./-]+$') { throw 'Invalid LVGL path' }
    $path = Join-Path $lvglRoot $file.path
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $file.sha256) {
        throw "LVGL mismatch: $($file.path)"
    }
}
Write-Output "PASS: $($lvgl.files.Count) LVGL files match $($lvgl.tag) at $($lvgl.commit)"
$network = Get-Content -Raw -LiteralPath (Join-Path $project 'network-lock.json') | ConvertFrom-Json
foreach ($component in $network.components) {
    foreach ($file in $component.files) {
        if ($file.path -match '\.\.' -or $file.path -notmatch '^[A-Za-z0-9_./-]+$') { throw 'Invalid network source path' }
        $path = Join-Path $project $file.path
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $file.sha256) {
            throw "Network source mismatch: $($file.path)"
        }
    }
    Write-Output "PASS: $($component.name), $($component.files.Count) locked files"
}
