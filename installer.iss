#define MyAppName "Darkroom"
#define MyAppPublisher "VioletGiraffe"
#define MyAppExeName "Darkroom.exe"
#define QuickroomName "Quickroom"
#define QuickroomExeName "Quickroom.exe"
#define IconDir "filetypes"
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
; Makes Setup notify the shell of the [Registry] associations, so icons appear without a logoff
ChangesAssociations=yes

SolidCompression=true
LZMANumBlockThreads=4
Compression=lzma2/ultra64
LZMAUseSeparateProcess=yes
LZMABlockSize=8192

[Files]
; Each app exe has its own entry so ignoreversion forces overwrite on same-version rebuilds. Being non-wildcard
; Sources, they also make a missing exe (e.g. a failed build) a hard compile error instead of a silent broken installer.
Source: "{#SourcePath}\dist\{#MyAppExeName}";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\dist\{#QuickroomExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\dist\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs; Excludes: "{#VCRedistExeName},{#MyAppExeName},{#QuickroomExeName}"
Source: "{#SourcePath}\dist\{#VCRedistExeName}";  DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#SourcePath}\LICENSE"; DestDir: "{app}"
Source: "{#SourcePath}\NOTICE";  DestDir: "{app}"
; Committed source assets, not build output, so they come from the repo rather than dist.
; ignoreversion: .ico files carry no version info, so an upgrade would otherwise compare timestamps.
Source: "{#SourcePath}\quickroom\res\filetypes\*.ico"; DestDir: "{app}\{#IconDir}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autoprograms}\{#QuickroomName}"; Filename: "{app}\{#QuickroomExeName}"
Name: "{autoprograms}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{autodesktop}\{#QuickroomName}"; Filename: "{app}\{#QuickroomExeName}"; Tasks: quickroomdesktopicon

[Tasks]
; Literal descriptions, not {cm:CreateDesktopIcon}: that message takes no parameter, so two tasks using it read identically.
Name: desktopicon; Description: "Create a desktop icon for &{#MyAppName}"; GroupDescription: {cm:AdditionalIcons};
Name: quickroomdesktopicon; Description: "Create a desktop icon for &{#QuickroomName}"; GroupDescription: {cm:AdditionalIcons};

[Registry]
; A file's icon comes from whichever ProgID currently handles its extension, so a distinct icon per format
; requires a distinct ProgID per format. Windows 8+ forbids claiming the default handler programmatically:
; these entries only make Quickroom eligible, and the [Run] entry sends the user to Settings to choose.
Root: HKA; Subkey: "Software\Classes\Quickroom.jpeg"; ValueType: string; ValueName: ""; ValueData: "JPEG Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Quickroom.jpeg\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#IconDir}\jpg.ico"
Root: HKA; Subkey: "Software\Classes\Quickroom.jpeg\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#QuickroomExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\Quickroom.png"; ValueType: string; ValueName: ""; ValueData: "PNG Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Quickroom.png\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#IconDir}\png.ico"
Root: HKA; Subkey: "Software\Classes\Quickroom.png\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#QuickroomExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\Quickroom.webp"; ValueType: string; ValueName: ""; ValueData: "WebP Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Quickroom.webp\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#IconDir}\webp.ico"
Root: HKA; Subkey: "Software\Classes\Quickroom.webp\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#QuickroomExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\Quickroom.tiff"; ValueType: string; ValueName: ""; ValueData: "TIFF Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Quickroom.tiff\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#IconDir}\tiff.ico"
Root: HKA; Subkey: "Software\Classes\Quickroom.tiff\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#QuickroomExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\Quickroom.gif"; ValueType: string; ValueName: ""; ValueData: "GIF Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Quickroom.gif\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#IconDir}\gif.ico"
Root: HKA; Subkey: "Software\Classes\Quickroom.gif\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#QuickroomExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\Quickroom.bmp"; ValueType: string; ValueName: ""; ValueData: "Bitmap Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Quickroom.bmp\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#IconDir}\bmp.ico"
Root: HKA; Subkey: "Software\Classes\Quickroom.bmp\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#QuickroomExeName}"" ""%1"""

; uninsdeletevalue, not uninsdeletekey: the extension keys are shared with every other image app, and we
; own only our own value inside them. .jfif is a JPEG alias, so it shares the JPEG ProgID and icon.
Root: HKA; Subkey: "Software\Classes\.jpg\OpenWithProgids";  ValueType: string; ValueName: "Quickroom.jpeg"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.jpeg\OpenWithProgids"; ValueType: string; ValueName: "Quickroom.jpeg"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.jfif\OpenWithProgids"; ValueType: string; ValueName: "Quickroom.jpeg"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.png\OpenWithProgids";  ValueType: string; ValueName: "Quickroom.png";  ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.webp\OpenWithProgids"; ValueType: string; ValueName: "Quickroom.webp"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.tif\OpenWithProgids";  ValueType: string; ValueName: "Quickroom.tiff"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.tiff\OpenWithProgids"; ValueType: string; ValueName: "Quickroom.tiff"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.gif\OpenWithProgids";  ValueType: string; ValueName: "Quickroom.gif";  ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.bmp\OpenWithProgids";  ValueType: string; ValueName: "Quickroom.bmp";  ValueData: ""; Flags: uninsdeletevalue

; Capabilities plus RegisteredApplications are what list Quickroom in Settings > Default apps at all.
Root: HKA; Subkey: "Software\{#QuickroomName}"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#QuickroomName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Fast image browser and viewer"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpg";  ValueData: "Quickroom.jpeg"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpeg"; ValueData: "Quickroom.jpeg"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jfif"; ValueData: "Quickroom.jpeg"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".png";  ValueData: "Quickroom.png"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webp"; ValueData: "Quickroom.webp"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tif";  ValueData: "Quickroom.tiff"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tiff"; ValueData: "Quickroom.tiff"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".gif";  ValueData: "Quickroom.gif"
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities\FileAssociations"; ValueType: string; ValueName: ".bmp";  ValueData: "Quickroom.bmp"
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "{#QuickroomName}"; ValueData: "Software\{#QuickroomName}\Capabilities"; Flags: uninsdeletevalue

; Deliberately no Applications\Quickroom.exe entry: it adds a second, identical-looking Quickroom to the
; Open With dialog, and choosing that one sets UserChoice to the application rather than to a ProgID. An
; application has one icon for every type it opens, so that silently defeats the per-type icons above.
; Quickroom still appears in Open With through the OpenWithProgids values.

[Run]
Filename: "{tmp}\{#VCRedistExeName}"; Parameters: "/install /quiet /norestart"; StatusMsg: Installing Microsoft C++ Runtime...; Flags: runhidden waituntilterminated skipifdoesntexist
Filename: "{app}\{#MyAppExeName}"; Description: {cm:LaunchProgram,{#MyAppName}}; Flags: nowait postinstall skipifsilent
; shellexec: ms-settings: is a URI, not an exe. runasoriginaluser: Setup is elevated and Settings will not
; launch under an elevated token. The registeredAppMachine query needs Windows 11 22H2; older builds ignore
; it and open the plain Default apps page.
Filename: "ms-settings:defaultapps?registeredAppMachine={#QuickroomName}"; Description: "Choose which image types &{#QuickroomName} opens"; Flags: shellexec nowait postinstall skipifsilent runasoriginaluser unchecked

[UninstallDelete]
Type: dirifempty; Name: "{app}\{#IconDir}"
Type: dirifempty; Name: "{app}"