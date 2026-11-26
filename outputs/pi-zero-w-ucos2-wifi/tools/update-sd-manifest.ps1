$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$sdRoot = Join-Path $project 'sdcard'
$names = @('bootcode.bin', 'start.elf', 'fixup.dat', 'LICENCE.broadcom', 'config.txt', 'kernel.img')
$lines = foreach ($name in $names) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $sdRoot $name)).Hash.ToLowerInvariant()
    "$hash *$name"
}
[IO.File]::WriteAllLines((Join-Path $sdRoot 'SHA256SUMS.txt'), [string[]]$lines, [Text.Encoding]::ASCII)
Write-Output 'Updated sdcard/SHA256SUMS.txt from current staged files.'
