param([ValidateRange(1024,65535)][int]$Port = 4096)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$version = '1.19.3'
$root = Join-Path $env:LOCALAPPDATA 'outershell-opencode'
$config = if ($env:XDG_CONFIG_HOME) { Join-Path $env:XDG_CONFIG_HOME 'opencode' } else { Join-Path $env:USERPROFILE '.config\opencode' }
$workspace = Join-Path $env:USERPROFILE 'server-admin'
$utf8 = New-Object System.Text.UTF8Encoding($false)
New-Item -ItemType Directory -Force $root, "$config\skills\outer-shell", $workspace | Out-Null
$listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $Port)
try { $listener.Start() } finally { $listener.Stop() }
$arch = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
switch ($arch) {
    'X64' { $asset='opencode-windows-x64-baseline.zip'; $digest='0d59113484bfe8d6dc146fc981f5c33faa8318303c183711b7a9387060b289e8' }
    'Arm64' { $asset='opencode-windows-arm64.zip'; $digest='c1b9daf18c97564b2d171703279621f7edc3613e70750d796ef78db87257ad89' }
    default { throw "OpenCode requires 64-bit Windows (x64 or ARM64). Found $arch." }
}
$binary = "$root\$version\opencode.exe"
if (!(Test-Path $binary)) {
    $stage = Join-Path $root ([Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $stage | Out-Null
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -TimeoutSec 600 "https://github.com/anomalyco/opencode/releases/download/v$version/$asset" -OutFile "$stage\download.zip"
        if ((Get-FileHash "$stage\download.zip" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) { throw 'OpenCode checksum verification failed.' }
        Expand-Archive "$stage\download.zip" "$stage\unpacked"
        & "$stage\unpacked\opencode.exe" --version
        if ($LASTEXITCODE) { throw 'OpenCode could not run on this server.' }
        New-Item -ItemType Directory -Force "$root\$version" | Out-Null
        Move-Item "$stage\unpacked\opencode.exe" $binary
    } finally { Remove-Item $stage -Recurse -Force }
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
$server = Start-Process -FilePath $binary -ArgumentList @('serve','--hostname','127.0.0.1','--port',"$Port") -WorkingDirectory $workspace -NoNewWindow -PassThru
try {
    $base = "http://127.0.0.1:$Port"
    $ready = $false
    for ($attempt = 0; $attempt -lt 120; $attempt++) {
        if ($server.HasExited) { throw 'OpenCode exited during startup.' }
        try { $health = Invoke-RestMethod "$base/global/health" -TimeoutSec 2; $ready = $health.healthy; if ($ready) { break } } catch { }
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
            try { $session = Invoke-RestMethod "$base/session/$id$query" -TimeoutSec 10 } catch { }
        }
    }
    if (!$session) {
        $session = Invoke-RestMethod "$base/session$query" -Method Post -ContentType 'application/json' -Body '{"title":"Server administration"}' -TimeoutSec 30
        if ($session.id -notmatch '^ses_[a-zA-Z0-9]+$') { throw 'Could not create server-admin session.' }
        [IO.File]::WriteAllText($sessionFile, $session.id, $utf8)
    }
    $encoded = [Convert]::ToBase64String($utf8.GetBytes($directory)).TrimEnd('=').Replace('+','-').Replace('/','_')
    $path = "/$encoded/session/$($session.id)"
    $backend = 'org.outershell.command.opencode'
    $values = @($backend, "${backend}:main", 'OpenCode', $path, '127.0.0.1', '', '', '', '')
    $stream = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($stream)
    foreach ($value in @(13,1,1,0,$Port)) { $writer.Write([uint16]$value) }
    $writer.Write((New-Object byte[] 72))
    for ($i=0; $i -lt $values.Count; $i++) {
        $bytes = $utf8.GetBytes($values[$i]); $offset = $stream.Length
        $stream.Position = 10 + $i * 8; $writer.Write([uint32]$offset); $writer.Write([uint32]$bytes.Length)
        $stream.Position = $offset; $writer.Write($bytes)
    }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value.ToLowerInvariant()
    $pipe = New-Object IO.Pipes.NamedPipeClientStream('.', "org.outershell.outershelld.$sid", [IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect(10000)
        $out = New-Object IO.BinaryWriter($pipe)
        $out.Write([uint32]$stream.Length); $out.Write($stream.ToArray()); $out.Flush()
        $reader = New-Object IO.BinaryReader($pipe)
        $length = $reader.ReadUInt32()
        if ($length -lt 6 -or $length -gt 1048576) { throw 'Invalid registry response.' }
        $response = $reader.ReadBytes($length)
        if ($response.Length -ne $length -or [BitConverter]::ToUInt16($response,0) -ne 100 -or [BitConverter]::ToUInt32($response,2) -ne 0) { throw 'Could not register the server-admin launcher.' }
    } finally { $pipe.Dispose(); $writer.Dispose(); $stream.Dispose() }
    Write-Output 'OpenCode server-admin workspace is ready.'
    $server.WaitForExit()
    if ($server.ExitCode) { throw "OpenCode exited with code $($server.ExitCode)." }
} finally {
    if (!$server.HasExited) { Stop-Process -Id $server.Id -Force }
    $server.Dispose()
}
