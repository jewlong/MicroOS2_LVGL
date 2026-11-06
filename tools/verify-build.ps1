param([string]$Cross = 'arm-none-eabi-')
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

Push-Location -LiteralPath $project
try {
    $elfPath = Join-Path $project 'build/kernel.elf'
    $imgPath = Join-Path $project 'build/kernel.img'
    $elf = [IO.File]::ReadAllBytes($elfPath)
    $img = [IO.File]::ReadAllBytes($imgPath)
    Assert-True ($elf.Length -ge 52) 'ELF header is missing.'
    Assert-True ([BitConverter]::ToUInt32($elf, 0) -eq 0x464c457f) 'Not ELF.'
    Assert-True ($elf[4] -eq 1 -and $elf[5] -eq 1) 'Expected ELF32 little endian.'
    Assert-True ([BitConverter]::ToUInt16($elf, 18) -eq 40) 'Expected ARM machine.'
    Assert-True ([BitConverter]::ToUInt32($elf, 24) -eq 0x8000) 'Entry is not 0x8000.'

    $nmTool = $Cross + 'nm'
    $undefined = @(& $nmTool -u $elfPath 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'nm failed.' }
    Assert-True ($undefined.Count -eq 0) 'Unresolved symbols found.'
    $nmLines = @(& $nmTool -n $elfPath)
    if ($LASTEXITCODE -ne 0) { throw 'nm failed.' }
    $symbols = @{}
    foreach ($line in $nmLines) {
        if ($line -match '^([0-9a-fA-F]+)\s+\w\s+(\S+)\s*$') {
            $symbols[$Matches[2]] = [Convert]::ToUInt32($Matches[1], 16)
        }
    }
    foreach ($name in @('_start', '__image_start', '__image_end', '__data_load',
        '__data_start', '__data_end', '__bss_start', '__bss_end', '__svc_stack_bottom',
        '__svc_stack_top', 'vector_template', 'vector_template_end', 'port_svc_handler', 'port_irq_handler', 'fatal_undefined', 'fatal_prefetch', 'fatal_data', 'fatal_reserved', 'fatal_fiq',
        'data_cookie', 'bss_cookie')) {
        Assert-True ($symbols.ContainsKey($name)) "Missing symbol: $name"
    }
    Assert-True ($symbols['_start'] -eq 0x8000) '_start mismatch.'
    Assert-True ($symbols['__image_start'] -eq 0x8000) 'Image base mismatch.'
    Assert-True ($img.Length -eq ($symbols['__image_end'] - 0x8000)) 'Flat image size mismatch.'
    Assert-True ($symbols['__data_load'] -eq $symbols['__data_start']) 'Expected RAM LMA == VMA.'
    Assert-True (($symbols['__bss_end'] - $symbols['__bss_start']) % 4 -eq 0) 'BSS word alignment.'
    Assert-True ($symbols['__bss_start'] -ge $symbols['__image_end']) 'BSS overlaps image.'
    $previousEnd = $symbols['__bss_end']
    foreach ($stackName in @('svc', 'irq', 'abt', 'und', 'fiq')) {
        $bottom = $symbols[('__' + $stackName + '_stack_bottom')]
        $top = $symbols[('__' + $stackName + '_stack_top')]
        $expectedSize = 2048
        if ($stackName -eq 'svc' -or $stackName -eq 'irq') { $expectedSize = 8192 }
        Assert-True ($bottom -ge $previousEnd) 'Stack overlap.'
        Assert-True ($top % 8 -eq 0) 'Exception stack alignment.'
        Assert-True (($top - $bottom) -eq $expectedSize) 'Exception stack size.'
        Assert-True ($top -le 0x01000000) 'Stack exceeds reserved RAM.'
        $previousEnd = $top
    }
    Assert-True ($symbols['bss_cookie'] -ge $symbols['__bss_start'] -and
                 $symbols['bss_cookie'] -lt $symbols['__bss_end']) 'BSS cookie is not in BSS.'
    $cookieOffset = [int]($symbols['data_cookie'] - 0x8000)
    Assert-True ([BitConverter]::ToUInt32($img, $cookieOffset) -eq 0x1234abcd) 'Data cookie absent from image.'
    Assert-True (($symbols['vector_template_end'] - $symbols['vector_template']) -eq 64) 'Vector size mismatch.'
    $vecOffset = [int]($symbols['vector_template'] - 0x8000)
    for ($i = 0; $i -lt 8; ++$i) {
        Assert-True ([BitConverter]::ToUInt32($img, $vecOffset + 4 * $i) -eq [uint32]0xe59ff018L) 'Bad vector instruction.'
        $targets = @('_start', 'fatal_undefined', 'port_svc_handler', 'fatal_prefetch',
                     'fatal_data', 'fatal_reserved', 'port_irq_handler', 'fatal_fiq')
        $target = $symbols[$targets[$i]]
        Assert-True ([BitConverter]::ToUInt32($img, $vecOffset + 32 + 4 * $i) -eq $target) 'Bad vector target.'
    }

    foreach ($name in @('framebuffer_init', 'framebuffer', 'fb_message', 'gui_task', 'gui_stack', 'draw_pixels', 'lv_init', 'lv_timer_handler', 'lv_obj_send_event', 'lv_display_flush_ready', 'lv_mem_test')) {
        Assert-True ($symbols.ContainsKey($name)) ("Missing HDMI symbol: " + $name)
    }
    Assert-True (($symbols['fb_message'] % 16) -eq 0) 'Mailbox message alignment.'
    Assert-True ($symbols['fb_message'] -ge $symbols['__bss_start'] -and
        ($symbols['fb_message'] + 156) -le $symbols['__bss_end']) 'Mailbox message must remain in BSS.'
    Assert-True (($symbols['gui_stack'] % 8) -eq 0) 'GUI task stack alignment.'
    $readelfTool = $Cross + 'readelf'
    & $readelfTool -h -A -l $elfPath
    if ($LASTEXITCODE -ne 0) { throw 'readelf failed.' }
    Write-Output "PASS: ARM ELF32, entry 0x8000, flat image $($img.Length) bytes."
    Write-Output 'PASS: no unresolved symbols; data/BSS, IRQ/SVC vectors and exception stacks checked.'
    Write-Output 'Hardware UART/SD boot has NOT been tested by this script.'
    Get-FileHash -Algorithm SHA256 -LiteralPath $imgPath
} finally {
    Pop-Location
}
