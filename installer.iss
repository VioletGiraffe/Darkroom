#define MyAppName "Darkroom"
#define MyAppPublisher "VioletGiraffe"
#define MyAppExeName "Darkroom.exe"
#define VCRedistExeName "vc_redist.x64.exe"
; Version is read from the built exe (which gets it from VERSION in app.pro) - single source of truth
#define MyAppVersion GetVersionNumbersString(AddBackslash(SourcePath) + "dist\" + MyAppExeName)

[Setup]
; Fixed install identity: must never change, or upgrades stop finding existing installs
AppId={{E39F5C26-279C-4902-A64A-7560BF1D159F}
AppName={#MyAppName}
AppPublisher={#MyAppPublisher}
AppVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=.
OutputBaseFilename={#MyAppName}

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}

SolidCompression=true
LZMANumBlockThreads=4
Compression=lzma2/ultra64
LZMAUseSeparateProcess=yes
LZMABlockSize=8192

[Files]
; Main exe has its own entry so ignoreversion forces overwrite on same-version rebuilds. Being a non-wildcard
; Source, it also makes a missing exe (e.g. a failed build) a hard compile error instead of a silent broken installer.
Source: "{#SourcePath}\dist\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\dist\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs; Excludes: "{#VCRedistExeName},{#MyAppExeName}"
Source: "{#SourcePath}\dist\{#VCRedistExeName}";  DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#SourcePath}\LICENSE"; DestDir: "{app}"
Source: "{#SourcePath}\NOTICE";  DestDir: "{app}"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autoprograms}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: desktopicon; Description: {cm:CreateDesktopIcon}; GroupDescription: {cm:AdditionalIcons};

[Run]
Filename: "{tmp}\{#VCRedistExeName}"; Parameters: "/install /quiet /norestart"; StatusMsg: Installing Microsoft C++ Runtime...; Flags: runhidden waituntilterminated skipifdoesntexist
Filename: "{app}\{#MyAppExeName}"; Description: {cm:LaunchProgram,{#MyAppName}}; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: dirifempty; Name: "{app}"