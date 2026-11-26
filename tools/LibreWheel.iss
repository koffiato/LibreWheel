; SPDX-License-Identifier: GPL-3.0-or-later
#define BuildX64 "..\build\setup-x64\Release"
#define BuildX86 "..\build\setup-x86\Release"
#ifndef AppVersion
#define AppVersion "0.1.0"
#endif

[Setup]
AppId={{50A98BE2-BF31-4855-85F5-15C8FC8B1B3C}
AppName=LibreWheel
AppVersion={#AppVersion}
AppPublisher=LibreWheel Contributors
AppPublisherURL=https://github.com/koffiato/LibreWheel
AppSupportURL=https://github.com/koffiato/LibreWheel/issues
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName=LibreWheel
VersionInfoProductVersion={#AppVersion}
VersionInfoDescription=LibreWheel Installer
DefaultDirName={localappdata}\Programs\LibreWheel
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
AppMutex=Local\LibreWheel.Tray
RestartApplications=no
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
UninstallDisplayIcon={app}\LibreWheelTray.exe
SetupIconFile=..\src\windows\LibreWheel.ico
OutputDir=..\build\installer
OutputBaseFilename=LibreWheel-Setup

[Files]
Source: "{#BuildX64}\LibreWheelTray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildX64}\librewheel.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildX64}\LibreWheelFF.dll"; DestDir: "{app}\x64"; Flags: ignoreversion
Source: "{#BuildX64}\LibreWheelOutput.exe"; DestDir: "{app}\x64"; Flags: ignoreversion
Source: "{#BuildX86}\LibreWheelFF.dll"; DestDir: "{app}\x86"; Flags: ignoreversion
Source: "{#BuildX86}\LibreWheelOutput.exe"; DestDir: "{app}\x86"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LibreWheel-G29-Custom.lut"; DestDir: "{localappdata}\LibreWheel"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "..\settings.example.ini"; DestDir: "{localappdata}\LibreWheel"; DestName: "settings.ini"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "Register-LibreWheel.ps1"; Flags: dontcopy
Source: "Register-LibreWheel.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\LibreWheel"; Filename: "{app}\LibreWheelTray.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\LibreWheelTray.exe"; Description: "Launch LibreWheel"; Flags: postinstall nowait skipifsilent

[Code]
const
  RegistrationScript = 'Register-LibreWheel.ps1';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'LibreWheelTray';
  LibreWheelClass = '{00EB836B-81CB-4D34-B0DF-16202A83BD4A}';
  ForceKey = 'System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_C24F\OEMForceFeedback';

var
  ConflictPage: TInputOptionWizardPage;
  HasConflictHKLM: Boolean;
  HasConflictHKCU: Boolean;
  ConflictDetails: String;
  RegistrationSkipped: Boolean;

function PowerShellPath: String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function DllArguments: String;
begin
  Result := ExpandConstant(' -Dll64 "{app}\x64\LibreWheelFF.dll" -Dll32 "{app}\x86\LibreWheelFF.dll"');
end;

function HasCmdParam(const Flag: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
  begin
    if CompareText(ParamStr(I), Flag) = 0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function HasConflictInHive(RootKey: Integer): Boolean;
var
  Clsid: String;
begin
  Result := False;
  if not RegKeyExists(RootKey, ForceKey) then
    Exit;
  if RegQueryStringValue(RootKey, ForceKey, 'CLSID', Clsid) and
     (CompareText(Clsid, LibreWheelClass) = 0) then
    Exit;
  Result := True;
end;

function ResolveComServer(const Clsid: String): String;
var
  SubKey: String;
begin
  Result := '';
  if Clsid = '' then
    Exit;
  SubKey := 'Software\Classes\CLSID\' + Clsid + '\InprocServer32';
  if RegQueryStringValue(HKLM64, SubKey, '', Result) and (Result <> '') then Exit;
  if RegQueryStringValue(HKLM32, SubKey, '', Result) and (Result <> '') then Exit;
  if RegQueryStringValue(HKCU64, SubKey, '', Result) and (Result <> '') then Exit;
  if RegQueryStringValue(HKCU32, SubKey, '', Result) and (Result <> '') then Exit;
  Result := '';
end;

function DescribeConflict(RootKey: Integer; const HiveLabel: String): String;
var
  Clsid, Server: String;
begin
  Result := '  - ' + HiveLabel;
  if RegQueryStringValue(RootKey, ForceKey, 'CLSID', Clsid) and (Clsid <> '') then
  begin
    Result := Result + ': ' + Clsid;
    Server := ResolveComServer(Clsid);
    if Server <> '' then
      Result := Result + ' (' + Server + ')';
  end;
end;

procedure RefreshConflicts;
begin
  HasConflictHKLM := HasConflictInHive(HKLM64);
  HasConflictHKCU := HasConflictInHive(HKCU64);
  ConflictDetails := '';
  if HasConflictHKLM then
    ConflictDetails := DescribeConflict(HKLM64, 'HKLM (machine-wide)');
  if HasConflictHKCU then
  begin
    if ConflictDetails <> '' then
      ConflictDetails := ConflictDetails + #13#10;
    ConflictDetails := ConflictDetails + DescribeConflict(HKCU64, 'HKCU (current user)');
  end;
end;

function HasAnyConflict: Boolean;
begin
  Result := HasConflictHKLM or HasConflictHKCU;
end;

function ShouldRemoveConflicts: Boolean;
begin
  if not HasAnyConflict then
  begin
    Result := False;
    Exit;
  end;
  if WizardSilent then
    Result := HasCmdParam('/REMOVELOGITECH')
  else
    Result := ConflictPage.SelectedValueIndex = 0;
end;

procedure InitializeWizard;
var
  Description: String;
begin
  RefreshConflicts;
  Description :=
    'An existing Logitech/OEM force-feedback registration for the G29 was detected:' + #13#10 +
    ConflictDetails + #13#10#13#10 +
    'LibreWheel''s DirectInput force-feedback driver will NOT work while this registration remains in place.' + #13#10 +
    'If removed, a .reg backup is automatically saved to %LOCALAPPDATA%\LibreWheel\backups\ before deletion.';
  if HasConflictHKLM and (not IsAdmin) then
    Description := Description + #13#10 +
      'Note: Removing the HKLM entry will prompt for Administrator permission (UAC).';

  ConflictPage := CreateInputOptionPage(
    wpSelectDir,
    'Conflicting G29 Force-Feedback Driver Detected',
    'Remove existing Logitech/OEM registry entry so LibreWheel can work?',
    Description,
    True,
    False);
  ConflictPage.Add('Remove existing Logitech/OEM registration and register LibreWheel (Recommended)');
  ConflictPage.Add('Keep existing registration (install files only — LibreWheel FFB will NOT work)');
  ConflictPage.SelectedValueIndex := 0;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = ConflictPage.ID) and (not HasAnyConflict);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = ConflictPage.ID) and (ConflictPage.SelectedValueIndex = 1) and (not WizardSilent) then
  begin
    Result := MsgBox(
      'Warning: LibreWheel''s DirectInput force-feedback driver will NOT work in games while the existing Logitech/OEM G29 registration remains in place.' + #13#10#13#10 +
      'LibreWheel files will be installed, but DirectInput registration will be skipped until you remove the conflicting registration and run Register-LibreWheel.ps1.' + #13#10#13#10 +
      'Continue without registering LibreWheel?',
      mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

procedure AppendMemo(var Memo: String; const Section, NewLine: String);
begin
  if Section = '' then
    Exit;
  if Memo <> '' then
    Memo := Memo + NewLine + NewLine;
  Memo := Memo + Section;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Section: String;
begin
  Result := '';
  AppendMemo(Result, MemoUserInfoInfo, NewLine);
  AppendMemo(Result, MemoDirInfo, NewLine);
  AppendMemo(Result, MemoTypeInfo, NewLine);
  AppendMemo(Result, MemoComponentsInfo, NewLine);
  AppendMemo(Result, MemoGroupInfo, NewLine);
  AppendMemo(Result, MemoTasksInfo, NewLine);
  if HasAnyConflict then
  begin
    if ShouldRemoveConflicts then
      Section := 'Logitech/OEM G29 registration:' + NewLine + Space +
        'Back up to %LOCALAPPDATA%\LibreWheel\backups\ and remove conflicting registration, then register LibreWheel'
    else
      Section := 'Logitech/OEM G29 registration:' + NewLine + Space +
        'Keep existing registration (WARNING: LibreWheel DirectInput registration will be skipped and FFB will not work)';
    AppendMemo(Result, Section, NewLine);
  end;
end;

function RunRegistration(const ScriptPath, Arguments: String): String;
var
  ExitCode, Line: Integer;
  Output: TExecOutput;
begin
  Result := '';
  if not ExecAndCaptureOutput(PowerShellPath,
    '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + ScriptPath + '" ' + Arguments,
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode, Output) then
  begin
    Result := 'Could not start DirectInput registration. ' + SysErrorMessage(ExitCode);
    Exit;
  end;
  for Line := 0 to GetArrayLength(Output.StdOut) - 1 do
    Log(Output.StdOut[Line]);
  for Line := 0 to GetArrayLength(Output.StdErr) - 1 do
    Log(Output.StdErr[Line]);
  if ExitCode <> 0 then
  begin
    Result := 'DirectInput registration failed (exit code ' + IntToStr(ExitCode) + ').';
    if GetArrayLength(Output.StdErr) > 0 then
      Result := Result + #13#10 + Output.StdErr[0];
  end;
end;

function RemoveConflicts(const ScriptPath: String): String;
var
  BackupDir, Args: String;
  ErrorCode: Integer;
begin
  Result := '';
  BackupDir := ExpandConstant('{localappdata}\LibreWheel\backups');
  ForceDirectories(BackupDir);

  if HasConflictHKCU then
  begin
    Result := RunRegistration(ScriptPath,
      '-Action RemoveExistingOem -Scope User -BackupDir "' + BackupDir + '"');
    if Result <> '' then
      Exit;
  end;

  if HasConflictHKLM then
  begin
    Args := '-Action RemoveExistingOem -Scope Machine -BackupDir "' + BackupDir + '"';
    if IsAdmin then
    begin
      Result := RunRegistration(ScriptPath, Args);
      if Result <> '' then
        Exit;
    end
    else
    begin
      if not ShellExec('runas', PowerShellPath,
        '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + ScriptPath + '" ' + Args,
        '', SW_HIDE, ewWaitUntilTerminated, ErrorCode) then
      begin
        Result := 'Administrator permission is required to remove the machine-wide (HKLM) Logitech/OEM G29 registration. ' +
          SysErrorMessage(ErrorCode);
        Exit;
      end;
      if HasConflictInHive(HKLM64) then
      begin
        Result := 'The machine-wide (HKLM) Logitech/OEM G29 registration could not be removed. ' +
          'Check permissions or run Register-LibreWheel.ps1 -Action RemoveExistingOem as Administrator.';
        Exit;
      end;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Args: String;
begin
  RefreshConflicts;
  if HasAnyConflict and (not ShouldRemoveConflicts) then
  begin
    RegistrationSkipped := True;
    Result := '';
    Exit;
  end;

  RegistrationSkipped := False;
  ExtractTemporaryFile(RegistrationScript);
  Args := '-Action InstallOrSwitch -ValidateOnly';
  if HasAnyConflict then
    Args := Args + ' -RemoveExistingOem';
  Result := RunRegistration(ExpandConstant('{tmp}\') + RegistrationScript, Args);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ScriptPath, ErrorMessage: String;
begin
  if CurStep = ssPostInstall then
  begin
    if RegistrationSkipped then
    begin
      Log('Skipped DirectInput registration because conflicting Logitech/OEM G29 registration was kept.');
      Exit;
    end;
    ScriptPath := ExpandConstant('{app}\') + RegistrationScript;
    if HasAnyConflict and ShouldRemoveConflicts then
    begin
      ErrorMessage := RemoveConflicts(ScriptPath);
      if ErrorMessage <> '' then
        RaiseException(ErrorMessage);
    end;
    ErrorMessage := RunRegistration(ScriptPath, '-Action InstallOrSwitch' + DllArguments);
    if ErrorMessage <> '' then
      RaiseException(ErrorMessage);
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and RegistrationSkipped then
  begin
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      'Note: DirectInput registration was skipped because the existing Logitech/OEM G29 registration was kept. ' +
      'LibreWheel force feedback will not work until that registration is removed and Register-LibreWheel.ps1 is run.';
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ErrorMessage, StartupCommand: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    ErrorMessage := RunRegistration(ExpandConstant('{app}\') + RegistrationScript,
      '-Action Remove' + DllArguments);
    if ErrorMessage <> '' then
      RaiseException(ErrorMessage);
    if RegQueryStringValue(HKCU, RunKey, RunValue, StartupCommand) and
       (CompareText(StartupCommand, ExpandConstant('"{app}\LibreWheelTray.exe"')) = 0) then
      RegDeleteValue(HKCU, RunKey, RunValue);
  end;
end;
