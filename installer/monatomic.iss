; Monatomic Audio Player — Inno Setup installer.
; Build:  ISCC.exe monatomic.iss   (after `python build.py --target win-x64`)
; Output: F:\Monatomic\release\MonatomicSetup-<ver>.exe
;
; The payload is dist\win-x64 minus the cef_cache* dirs (per-machine browser
; cache — never ship it). The app itself decides at runtime where the CEF
; cache lives: beside the exe when the dir is writable (portable zip), else
; %LOCALAPPDATA%\Monatomic (this installed layout). Library/settings/stems
; already live in %APPDATA%\Monatomic and survive uninstall.

#define MyAppName "Monatomic"
#define MyAppVersion "1.0.0"
#define MyAppExeName "monatomic.exe"
#define MyRepoURL "https://github.com/Tendai2404/Monatomic"

[Setup]
AppId={{8B1F3C57-6E1D-4A52-9C0A-2D5B7E4F1A90}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=Monatomic
AppPublisherURL={#MyRepoURL}
AppSupportURL={#MyRepoURL}/issues
AppUpdatesURL={#MyRepoURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=F:\Monatomic\LICENSE.md
OutputDir=F:\Monatomic\release
OutputBaseFilename=MonatomicSetup-{#MyAppVersion}
SetupIconFile=F:\Monatomic\assets\monatomic.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
LZMANumBlockThreads=4
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0.17763
VersionInfoVersion={#MyAppVersion}
VersionInfoDescription={#MyAppName} Setup
; If the compressed payload pushes the single exe past GitHub's 2 GB asset
; limit, flip these two on to emit setup.exe + .bin slices (each < 2 GB):
;DiskSpanning=yes
;DiskSliceSize=2000000000

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "F:\Monatomic\dist\win-x64\*"; DestDir: "{app}"; \
    Excludes: "cef_cache*,*.log,.mn_writeprobe"; \
    Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; \
    Flags: nowait postinstall skipifsilent
