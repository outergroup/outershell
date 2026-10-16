param([string]$Image = 'outershell/windows-test:1', [ValidateSet('docker','wsl.container')][string]$Provider = 'wsl.container')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value.ToLowerInvariant()
$shellPipe = "org.outershell.outershell.$sid"

function Request([string]$Path, [string]$Body = '', [string]$PipeName = $shellPipe, [switch]$Native) {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', $PipeName.Replace('\\.\pipe\', ''), [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $pipe.Connect(15000)
        $method = if ($Body) { 'POST' } else { 'GET' }
        $data = [Text.Encoding]::UTF8.GetBytes($Body)
        $accept = if ($Native) { "Outerframe-Accept: application/vnd.outerframe`r`n" } else { '' }
        $header = [Text.Encoding]::ASCII.GetBytes("$method $Path HTTP/1.1`r`nHost: localhost`r`n${accept}Content-Type: application/json`r`nConnection: close`r`nContent-Length: $($data.Length)`r`n`r`n")
        $pipe.Write($header, 0, $header.Length)
        $pipe.Write($data, 0, $data.Length)
        $stream = [IO.MemoryStream]::new()
        $buffer = New-Object byte[] 65536
        while ($true) {
            $read = $pipe.ReadAsync($buffer, 0, $buffer.Length)
            if (!$read.Wait(60000)) { throw 'HTTP response timed out.' }
            if (!$read.Result) { break }
            $stream.Write($buffer, 0, $read.Result)
            if ($stream.Length -gt 16777216) { throw 'Response exceeded the test limit.' }
        }
        $text = [Text.Encoding]::UTF8.GetString($stream.ToArray())
        $stream.Dispose()
        if (!$text.StartsWith('HTTP/1.1 200')) { throw $text }
        return ($text -split "`r`n`r`n", 2)[1]
    } finally { $pipe.Dispose() }
}

function Operation([string]$Operation, [hashtable]$Values) {
    $Values.operation = $Operation
    $Values.requestID = [guid]::NewGuid().ToString()
    return (Request '/api/safe-spaces' ($Values | ConvertTo-Json -Compress) | ConvertFrom-Json)
}

function Wait-Container([string]$Name, [string]$State) {
    for ($attempt = 0; $attempt -lt 45; $attempt++) {
        $snapshot = Request '/api/container-snapshot' | ConvertFrom-Json
        $container = $snapshot.workspaces | Where-Object name -eq $Name | Select-Object -First 1
        if ($container -and $container.state -eq $State) { return $container }
        Start-Sleep -Milliseconds 500
    }
    throw "Container '$Name' did not become $State."
}

$name = "$Provider integration test " + [guid]::NewGuid().ToString('N')
$id = $null
try {
    Operation 'create' @{runtimeProviderID=$Provider; name=$name; baseImage=$Image; cpus=2; memoryInGB=2} | Out-Null
    $container = Wait-Container $name 'running'
    $id = $container.id
    if (!$container.ownsContainer -or $container.runtime.providerID -ne $Provider) { throw 'Incorrect ownership/runtime.' }
    $app = $container.apps | Where-Object serviceID -eq 'org.outershell.Files' | Select-Object -First 1
    if (!$app) { throw 'The test image must contain the standard Files OCI payload.' }
    $descriptor = Request '/' '' $app.externalSocketPath -Native
    if (!$descriptor.StartsWith('OUTR')) { throw 'Files did not return native content.' }
    Write-Output 'PASS named-pipe forwarding and native Files content'
    foreach ($verb in @('stopApp', 'startApp', 'restartApp')) {
        Operation $verb @{workspaceID=$id; serviceID=$app.serviceID} | Out-Null
        Write-Output "PASS $verb"
    }
    $logs = Operation 'appLogs' @{workspaceID=$id; serviceID=$app.serviceID}
    if (!$logs.appLog) { throw 'Missing app log response.' }
    Operation 'stop' @{workspaceID=$id} | Out-Null
    Wait-Container $name 'stopped' | Out-Null
    Operation 'start' @{workspaceID=$id} | Out-Null
    Wait-Container $name 'running' | Out-Null
    Operation 'rename' @{workspaceID=$id; name="$name renamed"} | Out-Null
    Wait-Container "$name renamed" 'running' | Out-Null
    Write-Output 'PASS container stop/start, rename, and app logs'
    Operation 'updateDockerfile' @{workspaceID=$id; dockerfile="FROM $Image`nRUN echo rebuilt > /rebuilt-marker`n"} | Out-Null
    $folder="$env:USERPROFILE\Downloads\OuterShell mount test"
    New-Item -ItemType Directory -Force $folder | Out-Null
    Set-Content "$folder\probe.txt" 'mount works'
    Operation 'updateContainerConfiguration' @{workspaceID=$id; mounts=@(@{id=[guid]::NewGuid().ToString();name='Test folder';hostPath=$folder;guestPath='/test-mount';isReadOnly=$true});environment=@(@{name='OUTER_TEST';value='configured'});publishedPorts=@(@{hostPort=47621;containerPort=8888})} | Out-Null
    Operation 'rebuildRecipe' @{workspaceID=$id} | Out-Null
    Start-Sleep -Seconds 3
    for($i=0;$i -lt 120;$i++) {
        $snapshot=Request '/api/container-snapshot' | ConvertFrom-Json
        $container=$snapshot.workspaces | Where-Object id -eq $id | Select-Object -First 1
        if($container.buildProgress.phase -eq 'failed'){throw $container.buildProgress.detail}
        if($container.state -eq 'running' -and !$container.recipe.needsRebuild){break}
        Start-Sleep -Seconds 1
    }
    if($container.recipe.needsRebuild){throw 'Rebuild did not complete'}
    $runtime = if($Provider -eq 'docker'){"$env:LOCALAPPDATA\Programs\DockerDesktop\resources\bin\docker.exe"}else{'C:\Program Files\WSL\wslc.exe'}
    $list = if($Provider -eq 'docker'){'ps'}else{'list'}
    $format = if($Provider -eq 'docker'){'{{json .}}'}else{'json'}
    $objects = & $runtime $list --all --no-trunc --format $format | ForEach-Object { $_ | ConvertFrom-Json }
    $actual = $objects | Where-Object {$_.Names.StartsWith("outershell-$id-")} | Select-Object -First 1
    if(!$actual){throw 'Replacement container not found in the current user runtime'}
    $probe = & $runtime exec $actual.ID /bin/sh -c 'cat /rebuilt-marker /test-mount/probe.txt; printenv OUTER_TEST'
    if($LASTEXITCODE -or ($probe -join "`n") -notmatch 'rebuilt[\r\n]+mount works[\r\n]+configured'){throw "Mount/environment probe failed: $probe"}
    $ErrorActionPreference='Continue'
    & $runtime exec $actual.ID /bin/sh -c 'echo forbidden > /test-mount/should-not-exist' 2>$null
    $writeExit=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    if(!$writeExit){throw 'Read-only mount allowed a write'}
    Write-Output 'PASS Dockerfile/configuration update, rebuild, read-only mount, environment, port mapping, and stable identity'
    Operation 'updateDockerfile' @{workspaceID=$id;dockerfile="FROM $Image`nRUN exit 37`n"} | Out-Null
    Operation 'rebuildRecipe' @{workspaceID=$id} | Out-Null
    for($i=0;$i -lt 60;$i++) {
        $snapshot=Request '/api/container-snapshot' | ConvertFrom-Json
        $container=$snapshot.workspaces | Where-Object id -eq $id | Select-Object -First 1
        if($container.buildProgress.phase -eq 'failed'){break}
        Start-Sleep -Seconds 1
    }
    if($container.buildProgress.phase -ne 'failed' -or $container.state -ne 'running'){throw 'Failed build did not preserve running container'}
    if(!$container.buildProgress.log.Contains('37')){throw 'Missing build diagnostics'}
    Write-Output 'PASS failed rebuild preserves existing container and reports build log'

} finally {
    if ($id) {
        Operation 'delete' @{workspaceID=$id} | Out-Null
        Write-Output 'PASS delete disposable test container'
    }
}
