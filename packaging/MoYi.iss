#ifndef AppVersion
  #define AppVersion "0.4.2"
#endif

[Setup]
AppId={{9D2B29EE-665B-4F72-82B7-67191C49B08D}
AppName=墨弈六子棋
AppVersion={#AppVersion}
AppPublisher=zhehong268
AppPublisherURL=https://github.com/zhehong268/liuziqi
DefaultDirName={localappdata}\Programs\MoYi
DefaultGroupName=墨弈六子棋
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist\installer
OutputBaseFilename=MoYi-Setup-Windows-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\MoYi.exe
CloseApplications=yes

[Files]
Source: "..\dist\MoYi\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; Flags: unchecked

[Icons]
Name: "{group}\墨弈六子棋"; Filename: "{app}\MoYi.exe"
Name: "{autodesktop}\墨弈六子棋"; Filename: "{app}\MoYi.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\MoYi.exe"; Description: "打开墨弈六子棋"; Flags: nowait postinstall skipifsilent
