param(
    [switch]$UpdateTests,
    [string]$OutputDirectory = "$PSScriptRoot/../build/windows",
    [string]$CoreDirectory,
    [string]$ContainerBootstrapDirectory,
    [ValidateSet('arm64', 'x64', 'x86')][string]$Architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant(),
    [string]$VisualStudioRoot,
    [ValidateSet('', 'Files', 'Plaintext', 'Top', 'Firehose', 'Profile', 'Terminal')][string]$App = '',
    [string]$AppRoot,
    [string]$HelpersDirectory,
    [string]$OuterLoopRoot = "$PSScriptRoot/../../outerloop/OuterLoop",
    [string]$BundlesDirectory = "$PSScriptRoot/../build/outerframe/bundles",
    [string]$PublicBaseURL = 'https://outershell.org/outer-shell'
)
$ErrorActionPreference = 'Stop'
if ($App -and !$AppRoot) { $AppRoot = Join-Path $PSScriptRoot ('..\..\' + $(if ($App -eq 'Firehose') { 'Trace' } else { $App })) }
if ($AppRoot) { $AppRoot = [IO.Path]::GetFullPath($AppRoot) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
if (!$CoreDirectory) {
    if (!$VisualStudioRoot) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $VisualStudioRoot = & $vswhere -latest -products '*' -property installationPath
        if (!$VisualStudioRoot) { throw 'Visual Studio Build Tools was not found.' }
    }
    $CoreDirectory = Join-Path $OutputDirectory $(if ($App) { "native-$App-$Architecture" } else { "native-$Architecture" })
    $updateTestsValue = if ($UpdateTests) { 'ON' } else { 'OFF' }
    $triple = @{ arm64='aarch64-pc-windows-msvc'; x64='x86_64-pc-windows-msvc'; x86='i686-pc-windows-msvc' }[$Architecture]
    $cmake = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ninja = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    $llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
    $hostArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    $batch = Join-Path ([IO.Path]::GetTempPath()) ('outershell-build-' + [Guid]::NewGuid().ToString('N') + '.cmd')
    $text = @"
@echo off
call "$VisualStudioRoot\Common7\Tools\VsDevCmd.bat" -no_logo -arch=$Architecture -host_arch=$hostArchitecture
if errorlevel 1 exit /b %errorlevel%
set "PATH=$llvm;$ninja;%PATH%"
"$cmake" -S "$PSScriptRoot" -B "$CoreDirectory" -G Ninja -DOUTERSHELL_UPDATE_TESTS=$updateTestsValue -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_C_COMPILER_TARGET=$triple -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_CXX_COMPILER_TARGET=$triple -DOUTERSHELL_WINDOWS_APP="$App" -DAPP_ROOT="$AppRoot" -DOUTERSHELL_ARCHITECTURE=$Architecture -DOUTERSHELL_PUBLIC_BASE_URL="$PublicBaseURL"
if errorlevel 1 exit /b %errorlevel%
"$cmake" --build "$CoreDirectory" --parallel 4
exit /b %errorlevel%
"@
    try {
        [IO.File]::WriteAllText($batch, $text)
        & cmd.exe /d /c $batch
        if ($LASTEXITCODE) { throw 'Windows C build failed.' }
    } finally { Remove-Item -LiteralPath $batch -Force -ErrorAction SilentlyContinue }
}
if ($App) {
    $root = Join-Path $OutputDirectory $App
    if (Test-Path $root) { Remove-Item $root -Recurse -Force }
    New-Item -ItemType Directory -Force "$root/web" | Out-Null
    Copy-Item (Join-Path $CoreDirectory "$($App)Backend.exe") $root -Force
    Copy-Item "$AppRoot/Resources/$($App)Web/*" "$root/web" -Recurse -Force
    Copy-Item "$AppRoot/app-icon.png" "$root/app-icon.png" -Force
    if ($App -eq 'Top') {
        $index = Join-Path "$root/web" 'index.html'
        [IO.File]::WriteAllText($index, [IO.File]::ReadAllText($index).Replace('<html', '<html data-containers-only="true"'))
    }
    if ($HelpersDirectory -and (Test-Path $HelpersDirectory)) {
        Copy-Item $HelpersDirectory "$root/helpers" -Recurse -Force
    }
    if ($App -ne 'Terminal') {
        New-Item -ItemType Directory -Force "$root/bundles" | Out-Null
        foreach ($platform in @('macos-arm', 'macos-x86')) {
            Copy-Item (Join-Path $BundlesDirectory "$($App)Content.bundle.$platform.aar") "$root/bundles" -Force
        }
    }
    return
}
foreach ($file in @('OuterShell.exe', 'outerctl.exe')) {
    Copy-Item (Join-Path $CoreDirectory $file) $OutputDirectory -Force
}
foreach ($obsolete in @('OuterShell.Windows.Runtime.dll', 'outershell-core.dll')) {
    Remove-Item (Join-Path $OutputDirectory $obsolete) -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force "$OutputDirectory/bundles", "$OutputDirectory/web" | Out-Null
foreach ($platform in @('macos-arm', 'macos-x86')) {
    Copy-Item (Join-Path $BundlesDirectory "OuterShell.bundle.$platform.aar") "$OutputDirectory/bundles" -Force
}
Copy-Item "$PSScriptRoot/../Resources/OuterShellWeb/*" "$OutputDirectory/web" -Recurse -Force
Copy-Item "$PSScriptRoot/../app-icon.png" "$OutputDirectory/web/favicon.png" -Force
Copy-Item "$PSScriptRoot/install.ps1" $OutputDirectory -Force
if ($ContainerBootstrapDirectory) {
    New-Item -ItemType Directory -Force "$OutputDirectory/container-bootstrap" | Out-Null
    Copy-Item "$ContainerBootstrapDirectory/*" "$OutputDirectory/container-bootstrap" -Recurse -Force
}
