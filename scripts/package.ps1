param(
    [string]$Executable = 'build/distinct-browser.exe',
    [string]$Version = '0.3.1',
    [string]$DesktopValidation
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $projectRoot
try {
    $source = Get-Item -LiteralPath $Executable
    if ($source.VersionInfo.FileVersion -ne $Version) { throw 'Executable version does not match the package version' }
    $dirty = git status --porcelain
    if ($LASTEXITCODE -or $dirty) { throw 'Commit source changes before packaging a fixed version' }
    $commit = git rev-parse HEAD
    if ($LASTEXITCODE) { throw 'Cannot identify source commit' }
    $destination = Join-Path $projectRoot "artifacts/releases/v$Version"
    New-Item -ItemType Directory -Force $destination | Out-Null
    $binary = Join-Path $destination 'distinct-browser.exe'
    Copy-Item -LiteralPath $source.FullName -Destination $binary -Force
    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText((Join-Path $destination 'SHA256SUMS.txt'), "$hash  distinct-browser.exe`n", [System.Text.UTF8Encoding]::new($false))
    $metadata = [ordered]@{
        version = $Version
        platform = 'windows-11-x64'
        executable = 'distinct-browser.exe'
        size_bytes = (Get-Item -LiteralPath $binary).Length
        sha256 = $hash
        source_commit = $commit
        protocol_version = 1
        required_runtime = 'Microsoft Edge WebView2 Evergreen Runtime (138+; host input processing required)'
    }
    if ($DesktopValidation) {
        $validation = Get-Content -LiteralPath $DesktopValidation -Raw | ConvertFrom-Json
        if ($validation.executable_sha256 -ne $hash) { throw 'Desktop validation report does not match the executable' }
        $reportPath = Join-Path $destination 'desktop-validation.json'
        Copy-Item -LiteralPath $DesktopValidation -Destination $reportPath -Force
        $metadata.desktop_validation = 'desktop-validation.json'
        $reportHash = (Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash.ToLowerInvariant()
        [System.IO.File]::AppendAllText((Join-Path $destination 'SHA256SUMS.txt'), "$reportHash  desktop-validation.json`n", [System.Text.UTF8Encoding]::new($false))
    }
    $json = $metadata | ConvertTo-Json
    [System.IO.File]::WriteAllText((Join-Path $destination 'artifact.json'), "$json`n", [System.Text.UTF8Encoding]::new($false))
    Write-Host "Packaged $destination"
} finally { Pop-Location }
