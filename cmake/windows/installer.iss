; Installeur Windows du plugin (Inno Setup 6), compilé par .github/scripts/Package-Windows.ps1.
; Variables passées par ce script : AppVersion, SourceDir (release/<config>), RepoDir, OutputDir, OutputBase.
;
; Le plugin va dans le dossier des plugins tiers d'OBS (%ProgramData%\obs-studio\plugins, OBS 28+),
; pas dans le dossier d'installation d'OBS : une mise à jour d'OBS ne l'efface pas.

#define AppName "GetBetterCS Delay"
#define PluginId "snipewatch-obs-delay"

[Setup]
AppId={{AA761C66-CDE3-413E-A5BC-7113DD5596F4}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Phoenix Digital
AppPublisherURL=https://www.getbettercs.com
AppSupportURL=https://github.com/phoenixdigital-dev/snipewatch-obs-delay
AppUpdatesURL=https://github.com/phoenixdigital-dev/snipewatch-obs-delay/releases
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginId}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBase}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} (plugin OBS)
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoCompany=Phoenix Digital
VersionInfoDescription={#AppName} installer (OBS plugin)

[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[InstallDelete]
; Anciennes copies manuelles dans le dossier d'OBS : sinon OBS chargerait le plugin deux fois.
Type: files; Name: "{code:ObsDir}\obs-plugins\64bit\{#PluginId}.dll"
Type: files; Name: "{code:ObsDir}\obs-plugins\64bit\{#PluginId}.pdb"
Type: filesandordirs; Name: "{code:ObsDir}\data\obs-plugins\{#PluginId}"
; Mise à jour : on repart d'un dossier propre (fichiers retirés d'une version à l'autre).
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"

[Files]
Source: "{#SourceDir}\{#PluginId}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[CustomMessages]
fr.ObsRunning=OBS Studio est ouvert. Ferme-le, puis clique sur OK pour continuer (Annuler pour quitter).
en.ObsRunning=OBS Studio is running. Close it, then click OK to continue (Cancel to quit).

[Messages]
fr.FinishedLabel=Le plugin est installé.%n%nOuvre OBS, puis Outils → SnipeWatch Delay pour activer le mode délai et coller ton jeton plugin (www.getbettercs.com → Réglages → Délai).
en.FinishedLabel=The plugin is installed.%n%nOpen OBS, then Tools → SnipeWatch Delay to enable delay mode and paste your plugin token (www.getbettercs.com → Settings → Delay).

[Code]
{ Dossier d'installation d'OBS (écrit par son installeur), pour nettoyer une ancienne copie manuelle. }
function ObsDir(Param: String): String;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Result) then
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function ObsRunning(): Boolean;
var
  Locator, Service, Procs: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Procs := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := Procs.Count > 0;
  except
  end;
end;

{ OBS garde la DLL ouverte : on attend qu'il soit fermé, à l'installation comme à la désinstallation. }
function WaitForObsClosed(): Boolean;
begin
  Result := True;
  while ObsRunning() do
    if MsgBox(CustomMessage('ObsRunning'), mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function InitializeSetup(): Boolean;
begin
  Result := WaitForObsClosed();
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForObsClosed();
end;
