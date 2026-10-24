param([string]$Executable = 'build/distinct-browser.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $projectRoot
try {
    $Executable = [System.IO.Path]::GetFullPath($Executable)
    & (Join-Path (Split-Path $Executable -Parent) 'control-tests.exe')
    if ($LASTEXITCODE) { throw 'Control tests failed' }
    & python tests/integration.py $Executable
    if ($LASTEXITCODE) { throw 'Integration tests failed' }
    & python tests/protocol.py $Executable
    if ($LASTEXITCODE) { throw 'Protocol tests failed' }
    & python tests/failures.py $Executable
    if ($LASTEXITCODE) { throw 'Failure tests failed' }
} finally { Pop-Location }
