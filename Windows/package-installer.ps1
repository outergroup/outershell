param(
    [Parameter(Mandatory=$true)][string]$PayloadDirectory,
    [ValidateSet('x64','arm64','x86')][string]$Architecture = 'x64',
    [string]$Version = '0.1.0',
    [string]$Compiler = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    [string]$SignCommand,
    [switch]$DevelopmentUnsigned
)
$ErrorActionPreference = 'Stop'
if (!$SignCommand -and !$DevelopmentUnsigned) { throw 'Release packaging requires SignCommand. Use DevelopmentUnsigned only for local testing.' }
$PayloadDirectory = [IO.Path]::GetFullPath($PayloadDirectory)
$args = @("/DPayloadDirectory=$PayloadDirectory", "/DReleaseArchitecture=$Architecture", "/DReleaseVersion=$Version")
if ($SignCommand) {
    foreach ($name in @('OuterShell.exe','FilesBackend.exe')) {
        $signature = Get-AuthenticodeSignature (Join-Path $PayloadDirectory $name)
        if ($signature.Status -ne 'Valid') { throw "$name must be signed before release packaging." }
    }
    $args += '/DSignCommand=1'
    $args += "/Srelease=$SignCommand"
}
& $Compiler @args "$PSScriptRoot\OuterShell.iss"
if ($LASTEXITCODE) { throw "Installer compilation failed ($LASTEXITCODE)." }
$setup = Join-Path $PayloadDirectory "installer\outer-shell-windows-$Architecture-setup.exe"
if ($SignCommand -and (Get-AuthenticodeSignature $setup).Status -ne 'Valid') { throw 'The resulting setup executable does not have a valid signature.' }
Write-Output $setup
