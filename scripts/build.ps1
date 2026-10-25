param(
    [string]$MsvcRoot,
    [string]$WindowsSdkRoot,
    [switch]$SkipTests,
    [string]$OutputDirectory = 'build'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $projectRoot
try {
    # An existing x64 developer shell takes precedence. Otherwise locate VS or
    # an unpacked compiler; never install machine-wide components implicitly.
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        if (-not $MsvcRoot) {
            $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
            if (Test-Path $vswhere) {
                $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
                if ($vs) { $MsvcRoot = Join-Path $vs 'VC\Tools\MSVC' }
            }
            if (-not $MsvcRoot) { $MsvcRoot = Join-Path $projectRoot '.tools\msvc\VC\Tools\MSVC' }
        }
        if (-not (Test-Path (Join-Path $MsvcRoot 'include'))) {
            $MsvcRoot = (Get-ChildItem $MsvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
        }
        if (-not $MsvcRoot) { throw 'Install the MSVC C++ build tools, or pass -MsvcRoot.' }
        if (-not $WindowsSdkRoot) {
            $registry = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
            $WindowsSdkRoot = $registry.KitsRoot10
        }
        if (-not $WindowsSdkRoot) { throw 'Pass -WindowsSdkRoot with the Windows SDK 10/11 directory.' }
        $sdkVersion = (Get-ChildItem (Join-Path $WindowsSdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
        $env:PATH = "$MsvcRoot\bin\Hostx64\x64;$WindowsSdkRoot\bin\$sdkVersion\x64;$env:PATH"
        $env:INCLUDE = "$MsvcRoot\include;$WindowsSdkRoot\Include\$sdkVersion\ucrt;$WindowsSdkRoot\Include\$sdkVersion\shared;$WindowsSdkRoot\Include\$sdkVersion\um;$WindowsSdkRoot\Include\$sdkVersion\winrt;$WindowsSdkRoot\Include\$sdkVersion\cppwinrt"
        $env:LIB = "$MsvcRoot\lib\x64;$WindowsSdkRoot\Lib\$sdkVersion\ucrt\x64;$WindowsSdkRoot\Lib\$sdkVersion\um\x64"
    }
    $sdkRoot = Join-Path $projectRoot '.deps\webview2'
    $version = '1.0.3650.58'
    if (-not (Test-Path "$sdkRoot\build\native\include\WebView2.h")) {
        New-Item -ItemType Directory -Force .deps | Out-Null
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$version/microsoft.web.webview2.$version.nupkg" -OutFile .deps/webview2.zip
        $hash = (Get-FileHash .deps/webview2.zip -Algorithm SHA256).Hash
        if ($hash -ne '911A472128C82AC8BAA0C486C23342CC9DD6E7DC50D754E676726642CA065C60') { throw 'WebView2 SDK checksum mismatch' }
        Expand-Archive .deps/webview2.zip $sdkRoot -Force
    }
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    & rc.exe /nologo /I src /fo $OutputDirectory/app.res src/app.rc
    if ($LASTEXITCODE) { throw 'Resource compilation failed' }
    $arguments = @('/nologo', '/std:c++17', '/O2', '/MT', '/EHsc', '/W4', '/utf-8', '/permissive-',
        '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', "/I$sdkRoot\build\native\include",
        "/Fo$OutputDirectory/", "/Fe$OutputDirectory/distinct-browser.exe", 'src/main.cpp', 'src/host/host.cpp', 'src/browser/pages.cpp', 'src/browser/site_interactions.cpp', 'src/window/native_toolbar.cpp', "$OutputDirectory/app.res",
        '/link', '/SUBSYSTEM:WINDOWS', '/DYNAMICBASE', '/NXCOMPAT',
        "$sdkRoot\build\native\x64\WebView2LoaderStatic.lib", 'user32.lib', 'gdi32.lib', 'ole32.lib',
        'oleaut32.lib', 'shlwapi.lib', 'shell32.lib', 'version.lib', 'windowsapp.lib', 'crypt32.lib',
        'd2d1.lib', 'dwrite.lib', 'dwmapi.lib', 'comctl32.lib', 'advapi32.lib')
    & cl.exe @arguments
    if ($LASTEXITCODE) { throw 'C++ compilation failed' }
    if (-not $SkipTests) {
        & cl.exe /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /Fo$OutputDirectory/ /Fe$OutputDirectory/control-tests.exe tests/control_tests.cpp
        if ($LASTEXITCODE) { throw 'Control test compilation failed' }
        & "$OutputDirectory/control-tests.exe"
        if ($LASTEXITCODE) { throw 'Control tests failed' }
        & cl.exe /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /Fo$OutputDirectory/ /Fe$OutputDirectory/address-tests.exe tests/address_tests.cpp
        if ($LASTEXITCODE) { throw 'Address test compilation failed' }
        & "$OutputDirectory/address-tests.exe"
        if ($LASTEXITCODE) { throw 'Address tests failed' }
        & cl.exe /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Fo$OutputDirectory/ /Fe$OutputDirectory/page-input-tests.exe tests/page_input_tests.cpp
        if ($LASTEXITCODE) { throw 'Page input test compilation failed' }
        & "$OutputDirectory/page-input-tests.exe"
        if ($LASTEXITCODE) { throw 'Page input tests failed' }
        $lifecycleArguments = $arguments | ForEach-Object {
            if ($_ -eq "/Fe$OutputDirectory/distinct-browser.exe") { "/Fe$OutputDirectory/lifecycle-tests.exe" }
            elseif ($_ -eq '/SUBSYSTEM:WINDOWS') { '/SUBSYSTEM:CONSOLE' }
            elseif ($_ -eq 'src/main.cpp') { 'tests/lifecycle_tests.cpp'; 'src/main.cpp' }
            else { $_ }
        }
        & cl.exe @lifecycleArguments
        if ($LASTEXITCODE) { throw 'Page lifecycle test compilation failed' }
        & "$OutputDirectory/lifecycle-tests.exe"
        if ($LASTEXITCODE) { throw 'Page lifecycle tests failed' }
        & cl.exe /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Fo$OutputDirectory/ /Fe$OutputDirectory/toolbar-component.exe tests/toolbar_component.cpp src/window/native_toolbar.cpp $OutputDirectory/app.res /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib dwmapi.lib comctl32.lib advapi32.lib
        if ($LASTEXITCODE) { throw 'Toolbar component test compilation failed' }
    }
    Write-Host "Built $projectRoot\$OutputDirectory\distinct-browser.exe"
} finally { Pop-Location }
