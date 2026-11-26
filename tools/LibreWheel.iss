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
DefaultDirName={localappdata}\Programs\LibreWheel
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
AppMutex=Local\LibreWheel.Tray
RestartApplications=no
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
Source: "..\LibreWheel-G29-Custom.csv"; DestDir: "{localappdata}\LibreWheel"; Flags: onlyifdoesntexist uninsneveruninstall
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

function DllArguments: String;
begin
  Result := ExpandConstant(' -Dll64 "{app}\x64\LibreWheelFF.dll" -Dll32 "{app}\x86\LibreWheelFF.dll"');
end;

function RunRegistration(const ScriptPath, Arguments: String): String;
var
  ExitCode, Line: Integer;
  Output: TExecOutput;
begin
  Result := '';
  if not ExecAndCaptureOutput(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
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

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ExtractTemporaryFile(RegistrationScript);
  Result := RunRegistration(ExpandConstant('{tmp}\') + RegistrationScript,
    '-Action InstallOrSwitch -ValidateOnly');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ErrorMessage: String;
begin
  if CurStep = ssPostInstall then
  begin
    ErrorMessage := RunRegistration(ExpandConstant('{app}\') + RegistrationScript,
      '-Action InstallOrSwitch' + DllArguments);
    if ErrorMessage <> '' then
      RaiseException(ErrorMessage);
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
