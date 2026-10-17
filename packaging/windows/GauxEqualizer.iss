; Gaux Equalizer - instalador para Windows (Inno Setup 6)
; Lo compila package_windows.ps1:  ISCC /DAppVersion=x.y.z /DBuildDir=<...\Release> /DOutDir=<...> GauxEqualizer.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\..\build-package\GauxEqualizer_artefacts\Release"
#endif
#ifndef OutDir
  #define OutDir "..\..\dist"
#endif

[Setup]
AppId={{A9FBBEB2-C13C-4A04-9CBF-76CA6DA1A452}
AppName=Gaux Equalizer
AppVersion={#AppVersion}
AppVerName=Gaux Equalizer {#AppVersion}
AppPublisher=Aleksander Gaux
DefaultDirName={autopf}\Gaux\Gaux Equalizer
DisableDirPage=yes
DisableProgramGroupPage=yes
DefaultGroupName=Gaux Equalizer
UninstallDisplayName=Gaux Equalizer
UninstallDisplayIcon={app}\Gaux Equalizer.exe
OutputDir={#OutDir}
OutputBaseFilename=GauxEqualizer_{#AppVersion}_Windows_Setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
WizardStyle=modern
InfoBeforeFile=..\ANTES DE INSTALAR - Windows.txt
CloseApplications=yes

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[Types]
Name: "full"; Description: "Completa (VST3 + app independiente)"
Name: "plugin"; Description: "Solo el plugin VST3"
Name: "custom"; Description: "Personalizada"; Flags: iscustom

[Components]
Name: "vst3"; Description: "Plugin VST3 (Ableton Live, FL Studio, Cubase, Studio One, Reaper, Bitwig...)"; Types: full plugin custom; Flags: fixed
Name: "app"; Description: "App independiente (para probarlo sin DAW)"; Types: full

[InstallDelete]
; quitar restos de una version anterior antes de copiar la nueva
Type: filesandordirs; Name: "{commoncf64}\VST3\Gaux Equalizer.vst3"

[Files]
Source: "{#BuildDir}\VST3\Gaux Equalizer.vst3\*"; DestDir: "{commoncf64}\VST3\Gaux Equalizer.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: vst3
Source: "{#BuildDir}\Standalone\Gaux Equalizer.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: app
Source: "..\LICENCIA.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Gaux Equalizer"; Filename: "{app}\Gaux Equalizer.exe"; Components: app

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Gaux Equalizer.vst3"

[Messages]
es.FinishedLabel=Gaux Equalizer quedó instalado.%n%nAbre tu DAW y vuelve a escanear los plugins (en Ableton: Opciones > Preferencias > Plug-ins > activa "Usar carpeta de sistema VST3" > Volver a escanear).
