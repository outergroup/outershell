$ErrorActionPreference='Stop'
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$task="OuterShell-$sid"
$marker=Join-Path $env:LOCALAPPDATA 'OuterShell\last-update-check'
Stop-ScheduledTask $task
Start-Sleep -Seconds 1
Remove-Item $marker -Force -ErrorAction SilentlyContinue
Start-ScheduledTask $task
Start-Sleep -Seconds 5
if(Test-Path $marker){throw 'Startup triggered an update check'}
Write-Output 'PASS untouched startup does not check updates'
function Homepage {
 $pipe=New-Object IO.Pipes.NamedPipeClientStream('.',"org.outershell.outershell.$($sid.ToLower())",[IO.Pipes.PipeDirection]::InOut)
 $pipe.Connect(15000)
 $bytes=[Text.Encoding]::ASCII.GetBytes("GET /api/backends HTTP/1.1`r`nHost: localhost`r`nConnection: close`r`n`r`n")
 $pipe.Write($bytes,0,$bytes.Length);$pipe.Flush();$buffer=New-Object byte[] 16384
 while($pipe.Read($buffer,0,$buffer.Length) -gt 0){}
 $pipe.Dispose()
}
Homepage
if(!(Test-Path $marker)){throw 'Homepage did not initiate a check'}
$stamp=(Get-Item $marker).LastWriteTimeUtc
Start-Sleep -Seconds 3
Homepage
if((Get-Item $marker).LastWriteTimeUtc -ne $stamp){throw 'Repeat request bypassed daily throttle'}
Write-Output 'PASS homepage check and daily throttle'
Stop-ScheduledTask $task
Start-Sleep -Seconds 1
Start-ScheduledTask $task
Start-Sleep -Seconds 3
Homepage
if((Get-Item $marker).LastWriteTimeUtc -ne $stamp){throw 'Restart bypassed persisted daily throttle'}
Write-Output 'PASS daily throttle survives backend restart'
(Get-Item $marker).LastWriteTimeUtc=[DateTime]::UtcNow.AddDays(-2)
Homepage
if((Get-Item $marker).LastWriteTimeUtc -lt $stamp){throw 'Expired throttle did not permit check'}
Write-Output 'PASS later interaction permits next daily check'
