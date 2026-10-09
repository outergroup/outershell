param([string]$SourceDirectory = $PSScriptRoot, [switch]$Start, [switch]$SystemService)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$sid = $identity.User.Value
if ($SystemService -and !(New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Installing the system service requires an elevated PowerShell.' }
$destination = if ($SystemService) { Join-Path $env:ProgramFiles 'OuterShell' } else { Join-Path $env:LOCALAPPDATA 'OuterShell' }
$serviceName = 'OuterShell'
$sections = [Security.AccessControl.AccessControlSections]::Owner -bor [Security.AccessControl.AccessControlSections]::Access
if (Test-Path $destination) {
    if ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The installation directory must not be a reparse point.' }
}
New-Item -ItemType Directory -Force $destination | Out-Null
$acl = New-Object Security.AccessControl.DirectorySecurity
$acl.SetSecurityDescriptorSddlForm("O:${sid}D:P(A;OICI;FA;;;$sid)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", $sections)
[IO.Directory]::SetAccessControl($destination, $acl)
if ($SystemService) {
    $service = Get-Service $serviceName -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne 'Stopped') { Stop-Service $serviceName; $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20)) }
 } else {
    foreach ($taskName in @("OuterShell-$sid", "OuterShell-Files-$sid")) {
        if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
            Disable-ScheduledTask -TaskName $taskName | Out-Null
            Stop-ScheduledTask -TaskName $taskName
        }
    }
    Get-CimInstance Win32_Process -Filter "Name='OuterShell.exe' OR Name='FilesBackend.exe'" | Where-Object { $_.ExecutablePath -eq "$destination\OuterShell.exe" -or $_.ExecutablePath -eq "$destination\FilesBackend.exe" } | ForEach-Object { $process = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
        if ($process) { $process.Kill(); if (!$process.WaitForExit(10000)) { throw 'The existing app did not stop.' }; $process.Dispose() }
    }
}
foreach ($file in @('OuterShell.exe', 'FilesBackend.exe')) {
    for ($attempt = 0; ; $attempt++) {
        try { Copy-Item (Join-Path $SourceDirectory $file) $destination -Force; break }
        catch [IO.IOException], [UnauthorizedAccessException] { if ($attempt -ge 20) { throw }; Start-Sleep -Milliseconds 250 }
    }
}

foreach ($obsolete in @('OuterShell.Windows.Runtime.dll', 'outershell-core.dll')) {
    $oldFile = Join-Path $destination $obsolete
    for ($attempt = 0; Test-Path -LiteralPath $oldFile; $attempt++) {
        try { Remove-Item -LiteralPath $oldFile -Force }
        catch [IO.IOException], [UnauthorizedAccessException] {
            if ($attempt -ge 20) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
}
foreach ($folder in @('web', 'files-web', 'bundles')) {
    New-Item -ItemType Directory -Force (Join-Path $destination $folder) | Out-Null
    Copy-Item (Join-Path $SourceDirectory "$folder/*") (Join-Path $destination $folder) -Recurse -Force
}
$executable = Join-Path $destination 'OuterShell.exe'
if ($SystemService) {
    $command = '"' + $executable + '" --service --service-name ' + $serviceName
    if ($service) {
        $existing = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        $result = Invoke-CimMethod -InputObject $existing -MethodName Change -Arguments @{ PathName=$command; StartMode='Manual' }
        if ($result.ReturnValue) { throw "Could not update the service ($($result.ReturnValue))." }
    } else { New-Service -Name $serviceName -BinaryPathName $command -StartupType Manual -DisplayName 'Outer Shell' | Out-Null }
    & sc.exe triggerinfo $serviceName 'start/namedpipe/org.outershell.outershell.s-1-5-18' 'start/namedpipe/org.outershell.outershelld.s-1-5-18' | Out-Null
    if ($LASTEXITCODE) { throw 'Could not configure named-pipe service activation.' }
    & sc.exe failure $serviceName reset= 86400 actions= restart/5000/restart/15000/restart/60000 | Out-Null
    if ($LASTEXITCODE) { throw 'Could not configure service recovery.' }
    if ($Start) { Start-Service $serviceName }
} else {
    & $executable --initialize
    if ($LASTEXITCODE) { throw 'Could not initialize the HTTP named-pipe allowlist.' }
    $principal = New-ScheduledTaskPrincipal -UserId $identity.Name -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    foreach ($app in @(@{Name="OuterShell-$sid"; File='OuterShell.exe'}, @{Name="OuterShell-Files-$sid"; File='FilesBackend.exe'})) {
        $action = New-ScheduledTaskAction -Execute (Join-Path $destination $app.File) -WorkingDirectory $destination
        Register-ScheduledTask -TaskName $app.Name -Action $action -Principal $principal -Settings $settings -Force | Out-Null
        Enable-ScheduledTask -TaskName $app.Name | Out-Null
    }
    if ($Start) { Start-ScheduledTask -TaskName "OuterShell-$sid" }
}
Write-Output "Installed Outer Shell and Files in $destination"
