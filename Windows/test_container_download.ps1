$ErrorActionPreference='Stop'
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value.ToLowerInvariant()
$pipeName="org.outershell.outershell.$sid"
$id=[guid]::NewGuid().ToString()
$root=Join-Path $env:LOCALAPPDATA "OuterShell\container-transfers\$id"
$route="/api/container-downloads/$id/Experiment.outershell-container"
function Open-Request([string]$Method,[string]$Path) {
 $pipe=[IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
 try {
  $pipe.Connect(10000)
  $request=[Text.Encoding]::ASCII.GetBytes("$Method $Path HTTP/1.1`r`nHost: localhost`r`nConnection: close`r`n`r`n")
  $pipe.Write($request,0,$request.Length)
  $header=[Text.StringBuilder]::new()
  $one=New-Object byte[] 1
  while(!$header.ToString().EndsWith("`r`n`r`n")) {
   $read=$pipe.ReadAsync($one,0,1)
   if(!$read.Wait(10000) -or !$read.Result){throw 'Incomplete HTTP headers'}
   [void]$header.Append([char]$one[0])
   if($header.Length -gt 4096){throw 'Headers too large'}
  }
  return @{Pipe=$pipe;Header=$header.ToString()}
 } catch {$pipe.Dispose();throw}
}
try {
 New-Item -ItemType Directory $root | Out-Null
 $file=Join-Path $root 'container.outershell-container'
 $block=New-Object byte[] 65536
 for($i=0;$i -lt $block.Length;$i++){$block[$i]=$i%256}
 $writer=[IO.File]::Create($file)
 try {for($i=0;$i -lt 1024;$i++){$writer.Write($block,0,$block.Length)}} finally {$writer.Dispose()}
 $expected=(Get-FileHash $file -Algorithm SHA256).Hash
 $slow=Open-Request GET $route
 try {
  $watch=[Diagnostics.Stopwatch]::StartNew()
  $head=Open-Request HEAD $route
  try {
   if($head.Header -notmatch '^HTTP/1.1 200' -or $head.Header -notmatch 'Content-Length: 67108864' -or $head.Header -notmatch 'attachment; filename="Experiment.outershell-container"'){throw 'Incorrect HEAD response'}
   if($watch.Elapsed.TotalSeconds -gt 3){throw 'Stalled download blocked HEAD request'}
  } finally {$head.Pipe.Dispose()}
 } finally {$slow.Pipe.Dispose()}
 $response=Open-Request GET $route
 $hash=[Security.Cryptography.SHA256]::Create()
 $buffer=New-Object byte[] 65536
 $length=0
 try {
  if($response.Header -notmatch '^HTTP/1.1 200'){throw $response.Header}
  while($length -lt 67108864) {
   $read=$response.Pipe.ReadAsync($buffer,0,$buffer.Length)
   if(!$read.Wait(10000) -or !$read.Result){throw 'Truncated download'}
   [void]$hash.TransformBlock($buffer,0,$read.Result,$null,0)
   $length+=$read.Result
  }
  [void]$hash.TransformFinalBlock([byte[]]@(),0,0)
  if([BitConverter]::ToString($hash.Hash).Replace('-','') -ne $expected){throw 'Downloaded bytes differ'}
 } finally {$response.Pipe.Dispose();$hash.Dispose()}
 foreach($case in @(@('POST',$route,'405'),@('GET',"/api/container-downloads/$id/bad%0d%0aname",'400'),@('GET',"/api/container-downloads/$([guid]::NewGuid())/missing",'404'))) {
  $r=Open-Request $case[0] $case[1]
  try {if(!$r.Header.StartsWith("HTTP/1.1 $($case[2])")){throw ('Unexpected response for '+$case[0]+' '+$case[1]+': '+($r.Header -split "`r`n")[0])}} finally {$r.Pipe.Dispose()}
 }
 'PASS: Windows streamed 64 MiB exactly; HEAD metadata, slow-client isolation, cancellation, and invalid requests'
} finally {Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue}
