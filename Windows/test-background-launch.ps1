param([string]$InstallationDirectory = (Join-Path $env:LOCALAPPDATA 'OuterShell'))
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class BackendConsoleTest {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint processId);
}
'@
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
foreach ($name in @('OuterShell.exe', 'FilesBackend.exe')) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $InstallationDirectory $name))
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ([BitConverter]::ToUInt16($bytes, $pe + 24 + 68) -ne 2) { throw "$name is a console executable" }
}
Write-Output 'PASS both executables use the Windows GUI subsystem'
$info = New-Object Diagnostics.ProcessStartInfo
$info.FileName = Join-Path $InstallationDirectory 'OuterShell.exe'
$info.Arguments = '--pipe'
$info.UseShellExecute = $false
$info.RedirectStandardOutput = $true
$process = [Diagnostics.Process]::Start($info)
$output = $process.StandardOutput.ReadToEnd()
if (!$process.WaitForExit(10000) -or $process.ExitCode -ne 0 -or !$output.Contains($sid.ToLowerInvariant())) { throw 'Redirected CLI output failed' }
$process.Dispose()
Write-Output 'PASS command-line arguments and redirected output'
$probeDirectory = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $probeDirectory | Out-Null
$probe = Join-Path $probeDirectory 'ConsoleProbe.exe'
Add-Type -OutputAssembly $probe -OutputType WindowsApplication @'
using System;
using System.Runtime.InteropServices;
public static class ConsoleProbe {
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AttachConsole(uint processId);
    public static int Main(string[] args) {
        if (AttachConsole(uint.Parse(args[0]))) return 1;
        return Marshal.GetLastWin32Error() == 6 ? 0 : 2;
    }
}
'@
try {
foreach ($app in @(
    @{ Task="OuterShell-$sid"; Pipe="org.outershell.outershell.$($sid.ToLowerInvariant())" },
    @{ Task="OuterShell-Files-$sid"; Pipe="org.outershell.files.$($sid.ToLowerInvariant())" }
)) {
    Stop-ScheduledTask -TaskName $app.Task
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ((Get-ScheduledTask -TaskName $app.Task).State -eq 'Running') {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Task did not stop' }
        Start-Sleep -Milliseconds 100
    }
    Start-ScheduledTask -TaskName $app.Task
    $pipe = New-Object IO.Pipes.NamedPipeClientStream('.', $app.Pipe, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $pipe.Connect(15000)
        [uint32]$backendId = 0
        if (![BackendConsoleTest]::GetNamedPipeServerProcessId($pipe.SafePipeHandle.DangerousGetHandle(), [ref]$backendId)) { throw 'Cannot identify backend process' }
        $check = Start-Process -FilePath $probe -ArgumentList "$backendId" -Wait -PassThru
        if ($check.ExitCode -ne 0) { throw "$($app.Task) has a console or could not be inspected ($($check.ExitCode))" }
        $check.Dispose()
        $request = [Text.Encoding]::ASCII.GetBytes("GET / HTTP/1.1`r`nHost: localhost`r`nConnection: close`r`n`r`n")
        $pipe.Write($request, 0, $request.Length)
        $buffer = New-Object byte[] 4096
        $read = $pipe.ReadAsync($buffer, 0, $buffer.Length)
        if (!$read.Wait(15000)) { throw 'Backend HTTP response timed out' }
        $response = [Text.Encoding]::ASCII.GetString($buffer, 0, $read.Result)
        if (!$response.StartsWith('HTTP/1.1 200')) { throw "Backend returned: $response" }
        Write-Output "PASS $($app.Task): Task Scheduler launch, no console, HTTP 200"
    } finally { $pipe.Dispose() }
}
} finally { Remove-Item -LiteralPath $probeDirectory -Recurse -Force }
