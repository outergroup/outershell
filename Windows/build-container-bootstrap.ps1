param(
    [Parameter(Mandatory=$true)][string]$LinuxBinariesDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force "$OutputDirectory\bin" | Out-Null
foreach ($tool in @('outershelld', 'outerctl', 'outershell-socket-bridge')) {
    Copy-Item (Join-Path $LinuxBinariesDirectory $tool) "$OutputDirectory\bin\$tool" -Force
}
foreach ($file in @('Dockerfile', 'outershell-container-init')) {
    Copy-Item (Join-Path $PSScriptRoot "..\Container\Support\$file") (Join-Path $OutputDirectory $file) -Force
}
