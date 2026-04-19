; MuteDiscordDevice installer (Inno Setup 6)
;
; Build locally:  iscc /DMyAppVersion=1.0.0 installer.iss
; CI builds from .github/workflows/release.yml on v*.*.* tag push.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif

#define MyAppName        "MuteDiscordDevice"
#define MyAppExeName     "MuteDiscordDevice_Config.exe"
#define MyAppPublisher   "MuteDiscordDevice"
#define MyAppURL         "https://github.com/"
#define MyAppRunRegKey   "Software\Microsoft\Windows\CurrentVersion\Run"
#define MyAppRunRegValue "MuteDiscordDevice"

[Setup]
; Pin this GUID so future installs UPGRADE instead of producing parallel installs.
AppId={{8F6B2A3E-4C9D-4F1E-9B7C-2D4A6E8F1A3B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}
DefaultDirName={localappdata}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
OutputDir=Output
OutputBaseFilename=MuteDiscordDevice-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Start {#MyAppName} automatically when Windows starts"; GroupDescription: "Startup:"; Flags: checkedonce
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "x64\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Autostart entry; removed on uninstall regardless.
Root: HKCU; Subkey: "{#MyAppRunRegKey}"; ValueType: string; ValueName: "{#MyAppRunRegValue}"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]

; Note: devices.txt is intentionally NOT listed in [Files] or [UninstallDelete].
; The app creates it in {app} on first configuration; leaving it through an
; uninstall means a reinstall finds the user's previous configuration.
