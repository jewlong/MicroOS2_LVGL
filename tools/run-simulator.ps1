$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$venvPy = Join-Path $project '.venv\Scripts\python.exe'

Push-Location -LiteralPath $project
try {
    if (-not (Test-Path -LiteralPath $venvPy)) {
        Write-Output 'Creating Python virtual environment (.venv) and installing Unicorn ARM emulator...'
        if (Get-Command py.exe -ErrorAction SilentlyContinue) {
            & py.exe -m venv .venv
        } else {
            & python.exe -m venv .venv
        }
        & $venvPy -m pip install -r (Join-Path $project 'tests\requirements.txt')
    }
    if (-not (Test-Path -LiteralPath (Join-Path $project 'build\kernel.elf'))) {
        & (Join-Path $PSScriptRoot 'build-and-stage.ps1')
    }
    Write-Output 'Starting interactive uC/OS-II + LVGL + ViewGP simulator window...'
    & $venvPy (Join-Path $project 'tests\sim_window.py') @args
} finally {
    Pop-Location
}
