param([string]$Root = (Join-Path (Split-Path -Parent $PSScriptRoot) 'sdcard'))
$ErrorActionPreference = 'Stop'
$manifest = Join-Path $Root 'SHA256SUMS.txt'
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
    throw "Missing manifest: $manifest"
}
$count = 0
foreach ($line in Get-Content -LiteralPath $manifest) {
    if ($line -match '^([0-9a-fA-F]{64})\s+\*?([A-Za-z0-9_.-]+)$') {
        $expected = $Matches[1]
        $name = $Matches[2]
        $path = Join-Path $Root $name
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        if ($actual -ne $expected) { throw "SHA256 mismatch: $name" }
        Write-Output "PASS: $name"
        ++$count
    } elseif ($line.Trim().Length -gt 0) {
        throw "Invalid manifest line: $line"
    }
}
if ($count -ne 6) { throw "Expected six boot/license files; found $count." }
Write-Output 'PASS: file integrity only; FAT32/MBR and physical boot require separate checks.'
