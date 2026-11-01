#ifndef PayloadDirectory
  #error PayloadDirectory must name the built Windows release directory
#endif
#ifndef ReleaseVersion
  #define ReleaseVersion "0.1.0"
#endif
#ifndef ReleaseArchitecture
  #define ReleaseArchitecture "x64"
#endif
[Setup]
AppId=org.outershell.windows
AppName=Outer Shell
AppVersion={#ReleaseVersion}
VersionInfoVersion={#ReleaseVersion}.0
AppPublisher=Outer Group
AppPublisherURL=https://outershell.org
DefaultDirName={localappdata}\OuterShell
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#PayloadDirectory}\installer
OutputBaseFilename=outer-shell-windows-{#ReleaseArchitecture}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\OuterShell.exe
CloseApplications=no
RestartApplications=no
SetupLogging=yes
ChangesEnvironment=yes
#ifdef SignCommand
SignTool=release $f
SignedUninstaller=yes
#endif
#if ReleaseArchitecture == "arm64"
ArchitecturesAllowed=arm64
#elif ReleaseArchitecture == "x64"
ArchitecturesAllowed=x64compatible
#else
ArchitecturesAllowed=x86compatible
#endif
[Files]
Source: "{#PayloadDirectory}\container-bootstrap\*"; DestDir: "{app}\container-bootstrap"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#PayloadDirectory}\OuterShell.exe"; DestName: "SetupTasks.exe"; Flags: dontcopy
Source: "{#PayloadDirectory}\OuterShell.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDirectory}\outerctl.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#PayloadDirectory}\web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDirectory}\bundles\*"; DestDir: "{app}\bundles"; Flags: ignoreversion recursesubdirs createallsubdirs
[InstallDelete]
Type: files; Name: "{app}\FilesBackend.exe"
Type: files; Name: "{app}\PlaintextBackend.exe"
Type: files; Name: "{app}\TopBackend.exe"
Type: files; Name: "{app}\FirehoseBackend.exe"
Type: files; Name: "{app}\ProfileBackend.exe"
Type: files; Name: "{app}\TerminalBackend.exe"
Type: filesandordirs; Name: "{app}\files-web"
Type: filesandordirs; Name: "{app}\plaintext-web"
Type: filesandordirs; Name: "{app}\top-web"
Type: filesandordirs; Name: "{app}\firehose-web"
Type: filesandordirs; Name: "{app}\profile-web"
Type: filesandordirs; Name: "{app}\terminal-web"
Type: files; Name: "{app}\bundles\*Content.bundle.*.aar"
[UninstallDelete]
Type: filesandordirs; Name: "{app}\apps"
Type: files; Name: "{app}\*.old"
[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; Check: NeedsPathEntry(ExpandConstant('{app}\bin'))
[Code]
function NeedsPathEntry(Entry: String): Boolean;
var Path: String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path) then Path := '';
  Result := Pos(';' + Lowercase(Entry) + ';', ';' + Lowercase(Path) + ';') = 0;
end;
procedure RemovePathEntry(Entry: String);
var Path: String; Index: Integer;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path) then exit;
  Path := ';' + Path + ';';
  Index := Pos(';' + Lowercase(Entry) + ';', Lowercase(Path));
  if Index = 0 then exit;
  Delete(Path, Index, Length(Entry) + 1);
  Path := Copy(Path, 2, Length(Path) - 2);
  RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path);
end;
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then RemovePathEntry(ExpandConstant('{app}\bin'));
end;
function RunSetupOperation(Operation: String): Boolean;
var Code: Integer;
begin
  Result := Exec(ExpandConstant('{tmp}\SetupTasks.exe'), Operation, ExpandConstant('{tmp}'), SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;
function InitializeSetup(): Boolean;
begin
  Result := not IsAdmin;
  if not Result then
    SuppressibleMsgBox('Run Outer Shell setup as your ordinary Windows user, without administrator elevation. An elevated install cannot be uninstalled without administrator approval on this PC.', mbError, MB_OK, IDOK);
end;
function FileInUse(Path: String): Boolean;
var Stream: TFileStream;
begin
  Result := False;
  try
    Stream := TFileStream.Create(Path, fmOpenReadWrite or fmShareExclusive);
    Stream.Free;
  except
    Result := True;
  end;
end;
function MoveRunningExecutablesAside(): Boolean;
var Find: TFindRec; Directory, Path, Suffix: String;
begin
  Result := True;
  Directory := ExpandConstant('{app}\');
  if FindFirst(Directory + '*.old', Find) then begin
    try
      repeat DeleteFile(Directory + Find.Name); until not FindNext(Find);
    finally
      FindClose(Find);
    end;
  end;
  Suffix := '.' + GetDateTimeString('yyyymmddhhnnss', #0, #0) + '.old';
  if FindFirst(Directory + '*.exe', Find) then begin
    try
      repeat
        Path := Directory + Find.Name;
        if (Pos('unins', Lowercase(Find.Name)) <> 1) and FileInUse(Path) then
          Result := RenameFile(Path, Path + Suffix) and Result;
      until not FindNext(Find);
    finally
      FindClose(Find);
    end;
  end;
end;
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ExtractTemporaryFile('SetupTasks.exe');
  if FileExists(ExpandConstant('{app}\OuterShell.exe')) and not RunSetupOperation('--setup-stop') then
    Result := 'Could not stop the existing Outer Shell tasks. Close Outer Shell and try again.'
  else if not MoveRunningExecutablesAside() then
    Result := 'Could not replace Outer Shell programs that are still running. Close Terminal sessions and try again.';
end;
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if not RunSetupOperation('--setup-install') then
      RaiseException('Could not register the Outer Shell tasks. Run setup again to repair this installation.');
  end;
end;
function InitializeUninstall(): Boolean;
var Code: Integer;
begin
  if IsAdmin and (Pos(Lowercase(AddBackslash(ExpandConstant('{localappdata}'))), Lowercase(ExpandConstant('{app}'))) <> 1) then begin
    MsgBox('Uninstall Outer Shell while signed in as the Windows user who installed it.', mbError, MB_OK);
    Result := False;
    exit;
  end;
  Result := Exec(ExpandConstant('{app}\OuterShell.exe'), '--setup-remove', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
  if not Result then MsgBox('Could not remove the Outer Shell tasks. Close Outer Shell and try again.', mbError, MB_OK);
end;
