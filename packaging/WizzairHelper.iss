; Inno Setup 6 script of the WizzairHelper installer (DECISIONS D-38). The `dist` target
; (cmake/WizzPackaging.cmake, cmake/WizzDist.cmake) compiles it with ISCC after staging the
; portable package, and passes:
;
;   /DWizzVersion=X.Y.Z        project(VERSION) of CMakeLists.txt
;   /DWizzStageDir=<dir>       the staged files: exactly the contents of the portable ZIP
;   /DWizzOutputDir=<dir>      <build>/dist
;   /DWizzOutputName=<name>    WizzairHelper-X.Y.Z-win64-setup
;
; The installer and updater contract (docs/RELEASING.md):
; - Per user, no administrator rights: {localappdata}\Programs\WizzairHelper by default,
;   a Start-menu entry, an optional desktop icon, and an uninstaller (unins000.exe in the
;   install folder, Settings > Apps). An installed copy is recognised by unins000.exe next
;   to WizzairHelper.exe; the settings in %LOCALAPPDATA%\WizzairHelper stay on uninstall.
; - Icons (DECISIONS D-50): Setup and the uninstaller carry the application icon
;   (SetupIconFile, the committed .ico); the shortcuts and the Settings > Apps entry show
;   WizzairHelper.exe's own icon, its first icon group (no IconFilename; UninstallDisplayIcon).
; - AppMutex: WizzairHelper.exe holds the mutex WizzairHelper.AppMutex while it runs
;   (src/app/lib/app_mutex.hpp), so Setup and the uninstaller see a running copy.
; - The app's silent update runs
;     <setup.exe> /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS [/RELAUNCH]
;   and then exits. A silent Setup waits up to 30 s for every running copy to end (see
;   InitializeSetup); a copy still running after that ends Setup with exit code 1 and
;   nothing changed. With /RELAUNCH, Setup starts WizzairHelper.exe again after a
;   successful install; without it, a silent install starts nothing. An interactive
;   install offers "Launch WizzairHelper" on its last page as usual.

#ifndef WizzVersion
  #error Pass /DWizzVersion=X.Y.Z; build the installer with: cmake --build --preset dist
#endif
#ifndef WizzStageDir
  #error Pass /DWizzStageDir=<staged files>; build the installer with: cmake --build --preset dist
#endif
#ifndef WizzOutputDir
  #error Pass /DWizzOutputDir=<dir>; build the installer with: cmake --build --preset dist
#endif
#ifndef WizzOutputName
  #error Pass /DWizzOutputName=<file name without .exe>; build the installer with: cmake --build --preset dist
#endif

#define AppName "WizzairHelper"
#define AppExe "WizzairHelper.exe"
#define AppUrl "https://github.com/Giorgi-Apriashvili/WizzairHelper"

[Setup]
; Never change the AppId: updates and the uninstaller find an existing installation by it.
AppId={{B4128C94-AFA4-470F-B5B2-7FF8C4ED572E}
AppName={#AppName}
AppVersion={#WizzVersion}
AppVerName={#AppName} {#WizzVersion}
AppPublisher=Giorgi Apriashvili
AppCopyright=Copyright (c) 2026 Giorgi Apriashvili
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#WizzVersion}.0
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#AppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
; Relative to this script's folder (SourceDir); tools/make_app_icon.py writes it.
SetupIconFile=..\src\app\resources\icon\WizzairHelper.ico
; Windows 10 1903 or later, x64 (BUILDING.md; the app uses the system icu.dll).
MinVersion=10.0.18362
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
AppMutex=WizzairHelper.AppMutex
; Files in use are closed through the Restart Manager. Starting the app again is the job of
; /RELAUNCH alone, so a closed copy is never started twice.
CloseApplications=yes
RestartApplications=no
OutputDir={#WizzOutputDir}
OutputBaseFilename={#WizzOutputName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Exactly the files of the portable ZIP (the same staged folder).
Source: "{#WizzStageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; Interactive: the "Launch WizzairHelper" check box on the last page.
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent; Check: not RelaunchRequested
; /RELAUNCH (the app's own update): start the app again once the files are in place.
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Flags: nowait; Check: RelaunchRequested

[Code]
const
  AppMutexName = 'WizzairHelper.AppMutex';
  ExitWaitMs = 30000;
  PollMs = 250;

// True when Setup was started with the /RELAUNCH switch (any letter case, like Inno's own
// switches).
function RelaunchRequested(): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/RELAUNCH') = 0 then
      Result := True;
end;

// Runs before Setup's own AppMutex check, which in a silent run gives up at once (exit code 1).
// The app starts its update and then exits, so a silent Setup first waits for every running
// copy to end, for up to ExitWaitMs. An interactive Setup asks the user as usual.
function InitializeSetup(): Boolean;
var
  Waited: Integer;
begin
  if WizardSilent() then
  begin
    Waited := 0;
    while CheckForMutexes(AppMutexName) and (Waited < ExitWaitMs) do
    begin
      Sleep(PollMs);
      Waited := Waited + PollMs;
    end;
    if Waited > 0 then
      Log(Format('Waited %d ms for running copies of WizzairHelper to exit.', [Waited]));
  end;
  Result := True;
end;
