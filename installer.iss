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
#define ProgIdEntries(str ProgId, str TypeName, str IconStem) \
  'Root: HKA; Subkey: "Software\Classes\Quickroom.' + ProgId + '"; ValueType: string; ValueName: ""; ValueData: "' + TypeName + '"; Flags: uninsdeletekey' + NewLine + \
  'Root: HKA; Subkey: "Software\Classes\Quickroom.' + ProgId + '\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\' + IconDir + '\' + IconStem + '.ico"' + NewLine + \
  'Root: HKA; Subkey: "Software\Classes\Quickroom.' + ProgId + '\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\' + QuickroomExeName + '"" ""%1"""'

; uninsdeletevalue, not uninsdeletekey: the extension keys are shared with every other app that opens the
; type, and we own only our own value inside them.
#define SuffixEntries(str Suffix, str ProgId) \
  'Root: HKA; Subkey: "Software\Classes\.' + Suffix + '\OpenWithProgids"; ValueType: string; ValueName: "Quickroom.' + ProgId + '"; ValueData: ""; Flags: uninsdeletevalue' + NewLine + \
  'Root: HKA; Subkey: "Software\' + QuickroomName + '\Capabilities\FileAssociations"; ValueType: string; ValueName: ".' + Suffix + '"; ValueData: "Quickroom.' + ProgId + '"'

; Capabilities plus RegisteredApplications are what list Quickroom in Settings > Default apps at all.
Root: HKA; Subkey: "Software\{#QuickroomName}"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#QuickroomName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\{#QuickroomName}\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Fast image and video browser and viewer"
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "{#QuickroomName}"; ValueData: "Software\{#QuickroomName}\Capabilities"; Flags: uninsdeletevalue

; One ProgID per icon, then every suffix that maps to it.
; No .ico suffix, deliberately: Explorer draws each .ico from its own contents, and a type icon would replace that.
#emit ProgIdEntries("jpeg", "JPEG Image", "jpg")
#emit SuffixEntries("jpg",  "jpeg")
#emit SuffixEntries("jpeg", "jpeg")
#emit SuffixEntries("jfif", "jpeg")
#emit ProgIdEntries("png", "PNG Image", "png")
#emit SuffixEntries("png", "png")
#emit ProgIdEntries("webp", "WebP Image", "webp")
#emit SuffixEntries("webp", "webp")
#emit ProgIdEntries("tiff", "TIFF Image", "tiff")
#emit SuffixEntries("tif",  "tiff")
#emit SuffixEntries("tiff", "tiff")
#emit ProgIdEntries("gif", "GIF Image", "gif")
#emit SuffixEntries("gif", "gif")
#emit ProgIdEntries("bmp", "Bitmap Image", "bmp")
#emit SuffixEntries("bmp", "bmp")
#emit ProgIdEntries("svg", "SVG Image", "svg")
#emit SuffixEntries("svg", "svg")
#emit ProgIdEntries("mp4", "MP4 Video", "mp4")
#emit SuffixEntries("mp4", "mp4")
#emit SuffixEntries("m4v", "mp4")
#emit ProgIdEntries("mov", "QuickTime Movie", "mov")
#emit SuffixEntries("mov", "mov")
#emit ProgIdEntries("avi", "AVI Video", "avi")
#emit SuffixEntries("avi", "avi")
#emit ProgIdEntries("mkv", "Matroska Video", "mkv")
#emit SuffixEntries("mkv", "mkv")
#emit ProgIdEntries("webm", "WebM Video", "webm")
#emit SuffixEntries("webm", "webm")
#emit ProgIdEntries("wmv", "Windows Media Video", "wmv")
#emit SuffixEntries("wmv", "wmv")
#emit ProgIdEntries("mpeg", "MPEG Video", "mpg")
#emit SuffixEntries("mpg",  "mpeg")
#emit SuffixEntries("mpeg", "mpeg")
#emit ProgIdEntries("mts", "MPEG Transport Stream", "mts")
#emit SuffixEntries("m2ts", "mts")
#emit SuffixEntries("mts",  "mts")
#emit SuffixEntries("ts",   "mts")
#emit ProgIdEntries("flv", "Flash Video", "flv")
#emit SuffixEntries("flv", "flv")

; Deliberately no Applications\Quickroom.exe entry, in either form:
;   registered - a second, identical-looking Quickroom appears in the Open With dialog. Choosing it sets
;     UserChoice to the application, and an application has one icon for every type it opens.
;   NoOpenWith - a type whose UserChoice already is the application loses its Change button in Properties and
;     its row in Settings. Windows sets that UserChoice once the exe is picked by browsing.
; Quickroom is listed through the OpenWithProgids values.
; deletekey: a past version wrote this key with NoOpenWith. HKLM, not HKA: the per-user key is Windows' own.
Root: HKLM; Subkey: "Software\Classes\Applications\{#QuickroomExeName}"; Flags: deletekey

[Run]
Filename: "{tmp}\{#VCRedistExeName}"; Parameters: "/install /quiet /norestart"; StatusMsg: Installing Microsoft C++ Runtime...; Flags: runhidden waituntilterminated skipifdoesntexist
Filename: "{app}\{#MyAppExeName}"; Description: {cm:LaunchProgram,{#MyAppName}}; Flags: nowait postinstall skipifsilent
; shellexec: ms-settings: is a URI, not an exe. runasoriginaluser: Setup is elevated and Settings will not
; launch under an elevated token. The registeredAppMachine query needs Windows 11 22H2; older builds ignore
; it and open the plain Default apps page.
Filename: "ms-settings:defaultapps?registeredAppMachine={#QuickroomName}"; Description: "Choose which file types &{#QuickroomName} opens"; Flags: shellexec nowait postinstall skipifsilent runasoriginaluser unchecked

[UninstallDelete]
Type: dirifempty; Name: "{app}\{#IconDir}"
Type: dirifempty; Name: "{app}"