param(
    [Parameter(Mandatory=$true)][string]$LinuxBinariesDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force "$OutputDirectory\bin" | Out-Null
foreach ($tool in @('outershelld', 'outerctl', 'outer-socket-bridge')) {
    Copy-Item (Join-Path $LinuxBinariesDirectory $tool) "$OutputDirectory\bin\$tool" -Force
}
Copy-Item "$PSScriptRoot\container-bootstrap\*" $OutputDirectory -Force
