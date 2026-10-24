param([string]$Executable = 'build/distinct-browser.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $projectRoot
try {
    $Executable = [System.IO.Path]::GetFullPath($Executable)
    & (Join-Path (Split-Path $Executable -Parent) 'control-tests.exe')
    if ($LASTEXITCODE) { throw 'Control tests failed' }
    & (Join-Path (Split-Path $Executable -Parent) 'address-tests.exe')
    if ($LASTEXITCODE) { throw 'Address tests failed' }
    & python tests/integration.py $Executable
    if ($LASTEXITCODE) { throw 'Integration tests failed' }
    & python tests/navigation.py $Executable
    if ($LASTEXITCODE) { throw 'Navigation regression tests failed' }
    & python tests/shell.py $Executable
    if ($LASTEXITCODE) { throw 'Native shell tests failed' }
    & python tests/toolbar_component.py (Join-Path (Split-Path $Executable -Parent) 'toolbar-component.exe')
    if ($LASTEXITCODE) { throw 'Toolbar theme/DPI tests failed' }
    & python tests/protocol.py $Executable
    if ($LASTEXITCODE) { throw 'Protocol tests failed' }
    & python tests/failures.py $Executable
    if ($LASTEXITCODE) { throw 'Failure tests failed' }
} finally { Pop-Location }
