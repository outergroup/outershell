param(
    [string]$OutputDirectory = "$PSScriptRoot/../build/windows",
    [string]$CoreDirectory,
    [ValidateSet('arm64', 'x64', 'x86')][string]$Architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant(),
    [string]$VisualStudioRoot,
    [string]$FilesRoot,
    [string]$OuterLoopRoot = "$PSScriptRoot/../../outerloop/OuterLoop",
    [string]$BundlesDirectory = "$PSScriptRoot/../build/outerframe/bundles"
)
$ErrorActionPreference = 'Stop'
if (!$FilesRoot) { $FilesRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\Files')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
if (!$CoreDirectory) {
    if (!$VisualStudioRoot) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $VisualStudioRoot = & $vswhere -latest -products '*' -property installationPath
        if (!$VisualStudioRoot) { throw 'Visual Studio Build Tools was not found.' }
    }
    $CoreDirectory = Join-Path $OutputDirectory "native-$Architecture"
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
"$cmake" -S "$PSScriptRoot" -B "$CoreDirectory" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_C_COMPILER_TARGET=$triple -DFILES_ROOT="$FilesRoot"
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
foreach ($file in @('OuterShell.exe', 'FilesBackend.exe')) {
    Copy-Item (Join-Path $CoreDirectory $file) $OutputDirectory -Force
}
foreach ($obsolete in @('OuterShell.Windows.Runtime.dll', 'outershell-core.dll')) {
    Remove-Item (Join-Path $OutputDirectory $obsolete) -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force "$OutputDirectory/bundles" | Out-Null
foreach ($platform in @('macos-arm', 'macos-x86')) {
    Copy-Item (Join-Path $BundlesDirectory "OuterShell.bundle.$platform.aar") "$OutputDirectory/bundles" -Force
}
New-Item -ItemType Directory -Force "$OutputDirectory/web", "$OutputDirectory/files-web" | Out-Null
Copy-Item "$PSScriptRoot/../Resources/OuterShellWeb/*" "$OutputDirectory/web" -Recurse -Force
Copy-Item "$FilesRoot/Resources/FilesWeb/*" "$OutputDirectory/files-web" -Recurse -Force
Copy-Item "$PSScriptRoot/install.ps1" $OutputDirectory -Force
