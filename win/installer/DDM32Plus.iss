; SPDX-License-Identifier: GPL-3.0-or-later
; DDM32Plus-Setup.exe: per-user install of DDM32 Plus (no administrator prompt).
;
; Saved as UTF-8 with BOM (LF): Inno Setup reads the Turkish texts correctly only with the BOM.
; Build (Inno Setup 6.3 or newer; 7.1 checked, https://jrsoftware.org/isinfo.php), after the publish step in win/README.md:
;   "C:\Program Files\Inno Setup 7\ISCC.exe" win\installer\DDM32Plus.iss
; Output: win\publish\installer\DDM32Plus-Setup-<version>.exe
;
; Where things go:
;   {app} = %LOCALAPPDATA%\Programs\DDM32 Plus   DDM32Plus.exe (one self-contained file)
;   {app}\backups                                the radio's backups: made by the program, NEVER removed by the
;                                                uninstaller or by a program update; the uninstaller says where it is
;   {app}\settings.json, releases, updates       settings, downloaded versions, update staging: removed on uninstall
;   {app}\packs                                  data pack work files (the user's downloads, built packs): removed on uninstall
;   Up to 2.0.0-beta.2 the folders were yedekler, surumler, guncelleme, paketler; the program moves them at start.
; Uninstall: Settings > Apps (per-user entry, HKCU).

#define AppName "DDM32 Plus"
#define AppVersion "2.0.0-beta.5"
#define AppNumericVersion "2.0.0.0"
#define AppExe "DDM32Plus.exe"
#define AppPublisher "d0ukan · TA2DOU"
#define AppUrl "https://github.com/d0ukan/DDM32-Plus"

[Setup]
AppId={{AD326AA9-3719-4B7A-80B6-CB613B4F2E8B}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppNumericVersion}
VersionInfoProductName={#AppName}
; Per user: no admin prompt; {autopf} is %LOCALAPPDATA%\Programs in this mode
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableDirPage=auto
DisableProgramGroupPage=yes
DefaultGroupName={#AppName}
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
SetupIconFile=..\src\DDM32Plus\Assets\ddm32plus.ico
OutputDir=..\publish\installer
OutputBaseFilename=DDM32Plus-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809 or newer (the .NET 10 runtime inside the program needs it)
MinVersion=10.0.17763
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=yes
LanguageDetectionMethod=uilanguage

[Languages]
Name: "tr"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
tr.BackupsKept=Yedekleriniz silinmedi, burada duruyor:%n%n%1%n%nBu klasör telsizinizin kalibrasyonunu ve kanallarını içerir: silmeyin ve kimseyle paylaşmayın. Programı yeniden kurarsanız yedekler kaldığı yerden kullanılır.
en.BackupsKept=Your backups were not deleted; they stay here:%n%n%1%n%nThis folder holds your radio's calibration and channels: do not delete it and do not share it. If you install the program again, it uses these backups as they are.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Dirs]
; Created at install so the user can find it; never removed by the uninstaller
Name: "{app}\backups"; Flags: uninsneveruninstall

[Files]
Source: "..\publish\win-x64\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The program's own files besides the .exe; backups (and an old yedekler) are left alone
Type: files; Name: "{app}\settings.json"
Type: files; Name: "{app}\settings.tmp"
Type: filesandordirs; Name: "{app}\releases"
Type: filesandordirs; Name: "{app}\updates"
Type: filesandordirs; Name: "{app}\packs"
Type: filesandordirs; Name: "{app}\surumler"
Type: filesandordirs; Name: "{app}\guncelleme"
Type: filesandordirs; Name: "{app}\paketler"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Backups: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Backups := ExpandConstant('{app}\backups');
    if not DirExists(Backups) then
      Backups := ExpandConstant('{app}\yedekler');
    if DirExists(Backups) and not UninstallSilent then
      MsgBox(FmtMessage(CustomMessage('BackupsKept'), [Backups]), mbInformation, MB_OK);
  end;
end;
