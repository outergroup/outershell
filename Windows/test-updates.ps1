param(
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$Setup,
    [string]$Version = (Get-Content "$PSScriptRoot/../VERSION" -Raw).Trim()
)
$ErrorActionPreference='Stop'

& $fixture verify $setup
if($LASTEXITCODE){throw 'Verification fixture failed'}
& $fixture reject $fixture
if($LASTEXITCODE){throw 'Unsigned payload accepted'}
$bad=Join-Path $env:TEMP ('outershell-tampered-'+[guid]::NewGuid().ToString('N')+'.exe')
$bytes=[IO.File]::ReadAllBytes($setup); $bytes[512]=$bytes[512] -bxor 1; [IO.File]::WriteAllBytes($bad,$bytes)
& $fixture reject $bad
if($LASTEXITCODE){throw 'Tampered payload accepted'}
Remove-Item $bad
Write-Output 'PASS unsigned and tampered installer rejection'
$state=Join-Path $env:LOCALAPPDATA 'OuterShell'
$marker=Join-Path $state 'update-preservation-test.txt'
[IO.File]::WriteAllText($marker,'preserve')
& $fixture stage $setup
if($LASTEXITCODE){throw 'Staging fixture failed'}
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$task="OuterShell-Update-$sid"
$deadline=(Get-Date).AddMinutes(3)
do { Start-Sleep -Seconds 2; $info=Get-ScheduledTaskInfo $task; $running=(Get-ScheduledTask $task).State -eq 'Running' } while($running -and (Get-Date) -lt $deadline)
if($running -or $info.LastTaskResult){throw "Update task failed: $($info.LastTaskResult)"}
if((Get-Content $marker) -ne 'preserve'){throw 'User data not preserved'}
Remove-Item $marker
if((Get-ScheduledTask "OuterShell-$sid").State -ne 'Running'){throw 'Outer Shell was not restarted'}
Get-Content (Join-Path $state 'update-result.txt')
Write-Output 'PASS detached install, preserved state, and task restart'
function Request([string]$request){
 $pipe=New-Object IO.Pipes.NamedPipeClientStream('.',"org.outershell.outershell.$($sid.ToLower())",[IO.Pipes.PipeDirection]::InOut)
 $pipe.Connect(15000)
 $bytes=[Text.Encoding]::ASCII.GetBytes($request); $pipe.Write($bytes,0,$bytes.Length); $pipe.Flush()
 $stream=New-Object IO.MemoryStream; $buffer=New-Object byte[] 16384
 while(($n=$pipe.Read($buffer,0,$buffer.Length)) -gt 0){$stream.Write($buffer,0,$n)}
 $pipe.Dispose(); return [Text.Encoding]::UTF8.GetString($stream.ToArray())
}
$response=Request "GET /api/backends HTTP/1.1`r`nHost: localhost`r`nConnection: close`r`n`r`n"
if(!$response.Contains($Version) -or !$response.Contains('https://outershell.org/outer-shell')){throw 'Missing update version metadata'}
Write-Output 'PASS real HTTP backend version metadata'
$response=Request "POST /api/control?serviceID=org.outershell.OuterShell&operation=checkUpdate&scope=user HTTP/1.1`r`nHost: localhost`r`nContent-Length: 0`r`nConnection: close`r`n`r`n"
if(!$response.Contains('HTTP/1.1 200') -or !($response.Contains('update server returned HTTP 404') -or $response.Contains('up to date') -or $response.Contains('newer version'))){throw "Unexpected unpublished update response: $response"}
Write-Output 'PASS real HTTP update check response'
