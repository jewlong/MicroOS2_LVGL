param(
    [string]$Ssid,
    [ValidatePattern('^[A-Za-z]{2}$')][string]$Country = 'TW',
    [switch]$ScanOnly
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
function C-Bytes([byte[]]$Bytes) {
    $encoded = ($Bytes | ForEach-Object { '\x' + $_.ToString('x2') }) -join ''
    return '"' + $encoded + '"'
}
if ($ScanOnly) {
    $ssidLiteral = '""'
    $passwordLiteral = '""'
} else {
    if ([string]::IsNullOrEmpty($Ssid)) { $Ssid = Read-Host '2.4 GHz Wi-Fi SSID' }
    $ssidBytes = [Text.Encoding]::UTF8.GetBytes($Ssid)
    if ($ssidBytes.Length -lt 1 -or $ssidBytes.Length -gt 32 -or $Ssid.Contains([char]0)) {
        throw 'SSID must contain 1-32 UTF-8 bytes, without NUL.'
    }
    $secure = Read-Host 'WPA2-AES password (8-63 printable ASCII characters)' -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
        if ($plain.Length -lt 8 -or $plain.Length -gt 63 -or $plain -match '[^\x20-\x7e]') {
            throw 'Use an 8-63 character printable ASCII WPA2 passphrase.'
        }
        $ssidLiteral = C-Bytes $ssidBytes
        $passwordLiteral = C-Bytes ([Text.Encoding]::ASCII.GetBytes($plain))
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
        $plain = $null
        $secure.Dispose()
    }
}
$Country = $Country.ToUpperInvariant()
$lines = @(
    '/* Local configuration. Contains credentials; do not publish this file or configured binaries. */',
    ('#define WIFI_SSID ' + $ssidLiteral),
    ('#define WIFI_PASSWORD ' + $passwordLiteral),
    ("#define WIFI_COUNTRY_A '" + $Country[0] + "'"),
    ("#define WIFI_COUNTRY_B '" + $Country[1] + "'")
)
[IO.File]::WriteAllLines((Join-Path $project 'net/wifi_local.h'), $lines, [Text.Encoding]::ASCII)
$passwordLiteral = $null
Write-Output 'Saved net/wifi_local.h. Password was not printed.'
Write-Output 'Run tools/build-and-stage.ps1 next. Use the country where the board is operated.'
