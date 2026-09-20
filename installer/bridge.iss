; bridge.iss -- Cardboard++ Bridge web Setup (bridge only, fixed location).
;
; Tiny installer: downloads bridge-files.zip (exe + sidecar + models +
; embedded Python) from the latest GitHub release, extracts it to {app}.
; Built by scripts\compile-installer.ps1:
;   ISCC.exe bridge.iss /DAppVersion=<cargo> /DOutputDir=<abs>
; BridgeFilesUrl has a sane default so the script also compiles by hand.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef OutputDir
  #define OutputDir "dist"
#endif
#ifndef BridgeFilesUrl
  #define BridgeFilesUrl "https://github.com/gabrielbosse1/cardboardplusplus/releases/latest/download/bridge-files.zip"
#endif

[Setup]
AppId={{D800F60D-7D20-46CF-9C23-2BD37AF7D650}
AppName=Cardboard++ Bridge
AppVersion={#AppVersion}
AppVerName=Cardboard++ Bridge {#AppVersion}
AppPublisher=Cardboard++
AppPublisherURL=https://github.com/gabrielbosse1/cardboardplusplus
DefaultDirName={autopf}\CardboardPlusPlus
DisableDirPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=CardboardBridge-Setup-{#AppVersion}
WizardStyle=modern

[Icons]
Name: "{group}\Cardboard++ Bridge"; Filename: "{app}\cardboard-bridge-svc.exe"
Name: "{userdesktop}\Cardboard++ Bridge"; Filename: "{app}\cardboard-bridge-svc.exe"; Tasks: desktopicon

[Tasks]
Name: desktopicon; Description: "Create a &desktop icon"; Flags: unchecked

[Run]
Filename: "powershell"; Parameters: "-NoProfile -ExecutionPolicy Bypass -Command ""Expand-Archive -LiteralPath '{tmp}\bridge-files.zip' -DestinationPath '{app}' -Force"""; StatusMsg: "Unpacking bridge files..."; Flags: runhidden; Check: ZipDownloaded
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Cardboard++ Bridge UDP 42071"" dir=in action=allow protocol=UDP localport=42071"; Flags: runhidden; Check: not FirewallRuleExists('Cardboard++ Bridge UDP 42071')
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Cardboard++ Bridge UDP 42072"" dir=in action=allow protocol=UDP localport=42072"; Flags: runhidden; Check: not FirewallRuleExists('Cardboard++ Bridge UDP 42072')
Filename: "{app}\cardboard-bridge-svc.exe"; Description: "Launch Cardboard++ Bridge"; Flags: nowait postinstall skipifsilent; Check: SvcInstalled

[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Cardboard++ Bridge UDP 42071"""; Flags: runhidden
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Cardboard++ Bridge UDP 42072"""; Flags: runhidden

[Code]
var
  DownloadPage: TDownloadWizardPage;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), @OnDownloadProgress);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  if CurPageID = wpReady then begin
    DownloadPage.Clear;
    DownloadPage.Add('{#BridgeFilesUrl}', 'bridge-files.zip', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        Result := True;
      except
        if DownloadPage.AbortedByUser then
          Log('Bridge download aborted by user.')
        else
          SuppressibleMsgBox(AddPeriod(GetExceptionMessage) + ' Check the connection and run Setup again.', mbCriticalError, MB_OK, IDOK);
        Result := False;
      end;
    finally
      DownloadPage.Hide;
    end;
  end else
    Result := True;
end;

function ZipDownloaded: Boolean;
begin
  Result := FileExists(ExpandConstant('{tmp}\bridge-files.zip'));
end;

function SvcInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\cardboard-bridge-svc.exe'));
end;

function FirewallRuleExists(RuleName: String): Boolean;
var
  ResultCode: Integer;
begin
  Exec('netsh', 'advfirewall firewall show rule name="' + RuleName + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := ResultCode = 0;
end;
