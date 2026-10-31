& {
$shell = '__TTYD_SHELL__'
$pipe = '__TTYD_PIPE__'
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$version = '1.7.7'
$digest = 'e33a27501b10b96981335bcba938b1145c7f52551a343e72160f00ab71832b37'
$root = Join-Path $env:LOCALAPPDATA 'outershell-ttyd'
New-Item -ItemType Directory -Force $root | Out-Null
$binary = "$root\$version\ttyd.exe"
if (!(Test-Path $binary)) {
    $stage = Join-Path $root ([Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $stage | Out-Null
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -TimeoutSec 300 "https://github.com/tsl0922/ttyd/releases/download/$version/ttyd.win32.exe" -OutFile "$stage\ttyd.exe"
        if ((Get-FileHash "$stage\ttyd.exe" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) { throw 'ttyd checksum verification failed.' }
        & "$stage\ttyd.exe" --version
        if ($LASTEXITCODE) { throw 'ttyd could not run on this server.' }
        New-Item -ItemType Directory -Force (Split-Path $binary) | Out-Null
        if (!(Test-Path $binary)) { Move-Item "$stage\ttyd.exe" $binary }
    } finally { Remove-Item $stage -Recurse -Force }
}
switch ($shell) {
    'cmd' {
        $name = 'ttyd (Command Prompt)'
        $command = @("$env:SystemRoot\System32\cmd.exe")
    }
    'powershell' {
        $name = 'ttyd (PowerShell)'
        $powershell = (Get-Command pwsh.exe -ErrorAction SilentlyContinue).Source
        if (!$powershell) { $powershell = (Get-Command powershell.exe).Source }
        $command = @($powershell, '-NoLogo')
    }
    default { throw "Unsupported shell: $shell" }
}
$icon = Join-Path $root "icon-$shell.png"
[IO.File]::WriteAllBytes($icon, [Convert]::FromBase64String('__TTYD_ICON__'))
$backend = "org.outershell.command.ttyd-$shell"
$outerctl = if ($env:OUTERCTL_PATH) { $env:OUTERCTL_PATH } else { 'outerctl.exe' }
& $outerctl app upsert --backend $backend --frontend-id "${backend}:main" --name $name --socket-path $pipe --url / --icon-path $icon
if ($LASTEXITCODE) { Write-Warning 'Could not register the ttyd icon.' }
$bytes = New-Object byte[] 32
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$credential = 'outershell:' + [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
$listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$servers = @()
try {
    $arguments = @('--writable', '--interface', '127.0.0.1', '--port', "$port", '--credential', $credential, '--check-origin', '--cwd', "`"$env:USERPROFILE`"") + ($command | ForEach-Object { "`"$_`"" })
    $servers += Start-Process -FilePath $binary -ArgumentList $arguments -NoNewWindow -PassThru
    $env:OUTERSHELL_RELAY_HEADER = 'Authorization: Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($credential))
    try {
        $servers += Start-Process -FilePath $outerctl -ArgumentList @('relay', '--listen', $pipe, '--to', "127.0.0.1:$port", '--header-from-env', 'OUTERSHELL_RELAY_HEADER') -NoNewWindow -PassThru
    } finally { Remove-Item Env:\OUTERSHELL_RELAY_HEADER }
    foreach ($server in $servers) { $null = $server.Handle }
    Write-Output 'ttyd is ready.'
    while (!($servers | Where-Object { $_.HasExited })) { Start-Sleep 1 }
    $exited = $servers | Where-Object { $_.HasExited } | Select-Object -First 1
    throw "$(Split-Path -Leaf $exited.Path) exited with code $($exited.ExitCode)."
} finally {
    foreach ($server in $servers) {
        if (!$server.HasExited) { Stop-Process -Id $server.Id -Force }
        $server.Dispose()
    }
}
}
