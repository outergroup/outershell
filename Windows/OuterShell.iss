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
Source: "{#PayloadDirectory}\FilesBackend.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDirectory}\web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDirectory}\files-web\*"; DestDir: "{app}\files-web"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDirectory}\bundles\*"; DestDir: "{app}\bundles"; Flags: ignoreversion recursesubdirs createallsubdirs
[Code]
function RunSetupOperation(Operation: String): Boolean;
var Code: Integer;
begin
  Result := Exec(ExpandConstant('{tmp}\SetupTasks.exe'), Operation, ExpandConstant('{tmp}'), SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ExtractTemporaryFile('SetupTasks.exe');
  if FileExists(ExpandConstant('{app}\OuterShell.exe')) and not RunSetupOperation('--setup-stop') then
    Result := 'Could not stop the existing Outer Shell tasks. Close Outer Shell and try again.';
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
  if IsAdmin then begin
    MsgBox('Run this per-user uninstaller without administrator elevation.', mbError, MB_OK);
    Result := False;
    exit;
  end;
  Result := Exec(ExpandConstant('{app}\OuterShell.exe'), '--setup-remove', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
  if not Result then MsgBox('Could not remove the Outer Shell tasks. Close Outer Shell and try again.', mbError, MB_OK);
end;
