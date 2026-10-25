param(
    [string]$Executable = 'build/distinct-browser.exe',
    [string]$Version = '0.6.0',
    [string]$DesktopValidation,
    [string]$KeyboardStability
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
    foreach ($report in @(
        @{ Path=$DesktopValidation; Name='desktop-validation.json'; Key='desktop_validation' },
        @{ Path=$KeyboardStability; Name='keyboard-stability.json'; Key='keyboard_stability' }
    )) {
        if (-not $report.Path) { continue }
        $validation = Get-Content -LiteralPath $report.Path -Raw | ConvertFrom-Json
        if ($validation.executable_sha256 -ne $hash) { throw "Validation report $($report.Name) does not match the executable" }
        $reportPath = Join-Path $destination $report.Name
        Copy-Item -LiteralPath $report.Path -Destination $reportPath -Force
        $metadata[$report.Key] = $report.Name
        $reportHash = (Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash.ToLowerInvariant()
        [System.IO.File]::AppendAllText((Join-Path $destination 'SHA256SUMS.txt'), "$reportHash  $($report.Name)`n", [System.Text.UTF8Encoding]::new($false))
    }
    $json = $metadata | ConvertTo-Json
    [System.IO.File]::WriteAllText((Join-Path $destination 'artifact.json'), "$json`n", [System.Text.UTF8Encoding]::new($false))
    Write-Host "Packaged $destination"
} finally { Pop-Location }
