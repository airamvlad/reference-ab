; Inno Setup script for the Windows installer.
; Built by GitHub Actions: iscc /DAppVersion=0.3.0 installer\windows.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{B3C0B7B4-6C1E-4E0B-9B55-5E0C5B0A7A11}
AppName=Reference AB
AppVersion={#AppVersion}
AppVerName=Reference AB {#AppVersion}
AppPublisher=Reference AB
DefaultDirName={commoncf64}\VST3\Reference AB.vst3
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
UninstallFilesDir={commonappdata}\ReferenceAB
OutputDir=..\dist
OutputBaseFilename=ReferenceAB-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
WizardStyle=modern

[InstallDelete]
; Remove the previous version completely before copying the new one.
Type: filesandordirs; Name: "{commoncf64}\VST3\Reference AB.vst3"

[Files]
Source: "..\build\ReferenceAB_artefacts\Release\VST3\Reference AB.vst3\*"; DestDir: "{commoncf64}\VST3\Reference AB.vst3"; Flags: recursesubdirs ignoreversion

[Messages]
FinishedLabelNoIcons=Reference AB is installed. Open Bitwig and look for "Reference AB" in the plug-in list.
