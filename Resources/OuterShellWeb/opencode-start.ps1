& {
$mode = '__OPENCODE_MODE__'
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$version = '__OPENCODE_VERSION__'
$root = Join-Path $env:LOCALAPPDATA 'outershell-opencode'
$config = if ($env:XDG_CONFIG_HOME) { Join-Path $env:XDG_CONFIG_HOME 'opencode' } else { Join-Path $env:USERPROFILE '.config\opencode' }
$workspace = Join-Path $env:USERPROFILE 'server-admin'
$utf8 = New-Object System.Text.UTF8Encoding($false)
New-Item -ItemType Directory -Force $root, "$config\skills\outer-shell", $workspace | Out-Null
$arch = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
switch ($arch) {
    'X64' { $asset='opencode-windows-x64-baseline.zip'; $digest='__OPENCODE_SHA256_opencode-windows-x64-baseline.zip__' }
    'Arm64' { $asset='opencode-windows-arm64.zip'; $digest='__OPENCODE_SHA256_opencode-windows-arm64.zip__' }
    default { throw "OpenCode requires 64-bit Windows (x64 or ARM64). Found $arch." }
}
$binary = "$root\$version\opencode.exe"
if ($mode -eq 'update') {
    $binaries = @(Get-ChildItem -LiteralPath $root -Directory | ForEach-Object { Join-Path $_.FullName 'opencode.exe' } | Where-Object { Test-Path -LiteralPath $_ })
    if ($binaries.Count -ne 1) { throw 'Expected one Outer Shell OpenCode installation. Select the installation to update manually.' }
    $binary = $binaries[0]
}
if ($mode -eq 'update' -or !(Test-Path $binary)) {
    $stage = Join-Path $root ([Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $stage | Out-Null
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -TimeoutSec 600 "https://github.com/anomalyco/opencode/releases/download/v$version/$asset" -OutFile "$stage\download.zip"
        if ((Get-FileHash "$stage\download.zip" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) { throw 'OpenCode checksum verification failed.' }
        Expand-Archive "$stage\download.zip" "$stage\unpacked"
        & "$stage\unpacked\opencode.exe" --version
        if ($LASTEXITCODE) { throw 'OpenCode could not run on this server.' }
        New-Item -ItemType Directory -Force (Split-Path $binary) | Out-Null
        Move-Item -Force "$stage\unpacked\opencode.exe" $binary
    } finally { Remove-Item $stage -Recurse -Force }
}
if ($mode -eq 'update') {
    Write-Output "OpenCode $version installed. Start OpenCode from its app menu."
    return
}
if (!(Test-Path "$config\skills\outer-shell\SKILL.md")) {
    [IO.File]::WriteAllText("$config\skills\outer-shell\SKILL.md", @'
__OUTERSHELL_SKILL__
'@, $utf8)
}
if (!(Test-Path "$workspace\AGENTS.md")) {
    [IO.File]::WriteAllText("$workspace\AGENTS.md", @'
__OUTERSHELL_ADMIN__
'@, $utf8)
}
$rules = "$config\AGENTS.md"
if (!(Test-Path $rules) -or !([IO.File]::ReadAllText($rules).Contains('Outer Shell integration'))) {
    [IO.File]::AppendAllText($rules, "`n## Outer Shell integration`nUse the outer-shell skill to manage apps on this server. Read https://outershell.org/api/ for integration instructions.`n", $utf8)
}
Set-Location $workspace
if ((Get-Command git -ErrorAction SilentlyContinue) -and !(Test-Path .git)) {
    git init -q
    if ($LASTEXITCODE) { throw 'Could not initialize server-admin.' }
    git -c user.name='Outer Shell' -c user.email='outershell@localhost' -c commit.gpgsign=false commit --allow-empty -qm 'Initialize server administration workspace'
    if ($LASTEXITCODE) { throw 'Could not initialize server-admin history.' }
}
$pipe = '__OPENCODE_PIPE__'
$bytes = New-Object byte[] 32
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$password = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
$authorization = 'Basic ' + [Convert]::ToBase64String($utf8.GetBytes("opencode:$password"))
$headers = @{ Authorization = $authorization }
$listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
$listener.Start()
$Port = $listener.LocalEndpoint.Port
$listener.Stop()
$outerctl = if ($env:OUTERCTL_PATH) { $env:OUTERCTL_PATH } else { 'outerctl.exe' }
$servers = @()
$env:OPENCODE_SERVER_USERNAME = 'opencode'
$env:OPENCODE_SERVER_PASSWORD = $password
try {
    $servers += Start-Process -FilePath $binary -ArgumentList @('serve','--hostname','127.0.0.1','--port',"$Port") -WorkingDirectory $workspace -NoNewWindow -PassThru
} finally { Remove-Item Env:\OPENCODE_SERVER_PASSWORD }
try {
    $env:OUTERSHELL_RELAY_HEADER = "Authorization: $authorization"
    try {
        $servers += Start-Process -FilePath $outerctl -ArgumentList @('relay', '--listen', $pipe, '--to', "127.0.0.1:$Port", '--header-from-env', 'OUTERSHELL_RELAY_HEADER') -NoNewWindow -PassThru
    } finally { Remove-Item Env:\OUTERSHELL_RELAY_HEADER }
    foreach ($server in $servers) { $null = $server.Handle }
    $base = "http://127.0.0.1:$Port"
    $ready = $false
    for ($attempt = 0; $attempt -lt 120; $attempt++) {
        if ($servers | Where-Object { $_.HasExited }) { throw 'OpenCode exited during startup.' }
        try { $health = Invoke-RestMethod "$base/global/health" -Headers $headers -TimeoutSec 2; $ready = $health.healthy; if ($ready) { break } } catch { }
        Start-Sleep 1
    }
    if (!$ready) { throw 'OpenCode startup timed out.' }
    $directory = $workspace.Replace('\','/')
    $query = '?directory=' + [Uri]::EscapeDataString($directory)
    $sessionFile = "$root\server-admin-session"
    $session = $null
    if (Test-Path $sessionFile) {
        $id = [IO.File]::ReadAllText($sessionFile).Trim()
        if ($id -match '^ses_[a-zA-Z0-9]+$') {
            try { $session = Invoke-RestMethod "$base/session/$id$query" -Headers $headers -TimeoutSec 10 } catch { }
        }
    }
    if (!$session) {
        $session = Invoke-RestMethod "$base/session$query" -Method Post -Headers $headers -ContentType 'application/json' -Body '{"title":"Server administration"}' -TimeoutSec 30
        if ($session.id -notmatch '^ses_[a-zA-Z0-9]+$') { throw 'Could not create server-admin session.' }
        [IO.File]::WriteAllText($sessionFile, $session.id, $utf8)
    }
    $encoded = [Convert]::ToBase64String($utf8.GetBytes($directory)).TrimEnd('=').Replace('+','-').Replace('/','_')
    $path = "/$encoded/session/$($session.id)"
    $backend = 'org.outershell.command.opencode'
    & $outerctl app upsert --backend $backend --frontend-id "${backend}:main" --name 'OpenCode' --socket-path $pipe --url $path
    if ($LASTEXITCODE) { throw 'Could not register the server-admin launcher.' }
    Write-Output 'OpenCode server-admin workspace is ready.'
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
