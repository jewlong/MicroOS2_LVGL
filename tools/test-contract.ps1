param(
    [string]$PiHost = "",
    [string]$Token = "",
    [int]$SoakRuns = 0
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot

function Test-JsonContract([string]$FilePath) {
    if (-not (Test-Path -LiteralPath $FilePath)) { throw "Missing fixture: $FilePath" }
    $raw = [IO.File]::ReadAllText($FilePath, [Text.Encoding]::UTF8)
    $bytes = [Text.Encoding]::UTF8.GetByteCount($raw)
    $obj = $raw | ConvertFrom-Json

    if ($obj.schemaVersion -ne 1) { throw "${FilePath}: schemaVersion must be 1" }
    $screens = @($obj.screens).Count
    $vars = @($obj.variables).Count
    $devs = if ($obj.devices) { @($obj.devices).Count } else { 0 }
    $assets = if ($obj.assets) { @($obj.assets).Count } else { 0 }
    $areas = if ($obj.alarmAreas) { @($obj.alarmAreas).Count } else { 0 }
    $els = 0
    foreach ($s in $obj.screens) { $els += @($s.elements).Count }

    if ($screens -lt 1 -or $screens -gt 32) { throw "${FilePath}: screens=$screens out of 1..32" }
    if ($vars -gt 256) { throw "${FilePath}: variables=$vars > 256" }
    if ($els -gt 768) { throw "${FilePath}: elements=$els > 768" }
    if ($devs -gt 16) { throw "${FilePath}: devices=$devs > 16" }
    if ($assets -gt 32) { throw "${FilePath}: assets=$assets > 32" }
    if ($areas -gt 32) { throw "${FilePath}: alarmAreas=$areas > 32" }
    if ($bytes -gt 8388608) { throw "${FilePath}: size=$bytes > 8 MiB" }

    Write-Output "PASS contract: $(Split-Path -Leaf $FilePath) ($bytes B | scr=$screens el=$els var=$vars dev=$devs asset=$assets)"
}

Test-JsonContract (Join-Path $project 'sdcard\project.json')
Test-JsonContract (Join-Path $project 'tests\fixtures\contract_full.json')

if ($PiHost -ne "") {
    $base = if ($PiHost -match ':') { $PiHost } else { "${PiHost}:8080" }
    $headers = @{ 'Content-Type' = 'application/json' }
    if ($Token -ne "") { $headers['X-Ops-Token'] = $Token }

    $st0 = Invoke-RestMethod -Uri "http://$base/api/status" -Method Get
    Write-Output "Pi Online: $($st0.platform) | heapFree=$($st0.lvglHeapFree) | project=$($st0.project.name)"

    if ($SoakRuns -gt 0) {
        $f1 = [IO.File]::ReadAllText((Join-Path $project 'tests\fixtures\contract_full.json'), [Text.Encoding]::UTF8)
        $f2 = [IO.File]::ReadAllText((Join-Path $project 'sdcard\project.json'), [Text.Encoding]::UTF8)
        for ($i = 1; $i -le $SoakRuns; $i++) {
            $body = if (($i % 2) -eq 1) { $f1 } else { $f2 }
            $r = Invoke-RestMethod -Uri "http://$base/api/project" -Method Post -Headers $headers -Body $body
            Start-Sleep -Milliseconds 600
            $st = Invoke-RestMethod -Uri "http://$base/api/status" -Method Get
            Write-Output "Soak #$i/$SoakRuns ok=$($r.ok) heapFree=$($st.lvglHeapFree) warn=$($st.parseWarningCount) otaErr='$($st.lastOtaError)'"
        }
    }
}
