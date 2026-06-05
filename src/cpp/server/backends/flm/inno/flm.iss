[Setup]

; Basic installer configuration for flm

AppName=flm

AppVersion=0.9.43

AppPublisher=FastFlowLM

AppPublisherURL=www.fastflowlm.com

; DefaultDirName={localappdata}\flm
; PrivilegesRequired=lowest
; PrivilegesRequiredOverridesAllowed=dialog

DefaultDirName={pf64}\flm

DefaultGroupName=flm

DisableProgramGroupPage=no

OutputBaseFilename=flm-setup

Compression=lzma

SolidCompression=yes

LicenseFile=terms.txt

ChangesEnvironment=yes

; Icon configuration to preserve original background
SetupIconFile=logo.ico

UninstallDisplayIcon={app}\logo.ico

; Force icon usage without transparency effects
; UsePreviousAppDir=no

; SignTool=FLM_INC


[Files]

; Main executable

Source: "flm.exe"; DestDir: "{app}"; Flags: ignoreversion

; Required DLL

Source: "libcurl.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "llama_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "gemma_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "gemma_text_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "gemma_embedding.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "gpt_oss_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "lfm2_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "phi4_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "qwen2_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "qwen2vl_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "qwen3_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "qwen3vl_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "qwen3_5vl_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "nanbeige_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "gemma4e_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "lm_head.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "dequant.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "whisper_npu.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "gemm.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "q4_npu_eXpress.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "mha.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "abseil_dll.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "avcodec-61.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "avdevice-61.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "avfilter-10.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "avformat-61.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "avutil-59.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libprotobuf-lite.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libprotobuf.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libprotoc.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "swresample-5.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "swscale-8.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "zlib1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libfftw3-3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libfftw3f-3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libfftw3l-3.dll"; DestDir: "{app}"; Flags: ignoreversion

; Application icon (used for shortcuts)

Source: "logo.ico"; DestDir: "{app}"; Flags: ignoreversion

Source: "model_list.json"; DestDir: "{app}"; Flags: ignoreversion

; xclbins directory - recursively include all files
Source: "..\xclbins\*"; DestDir: "{app}\xclbins"; Flags: ignoreversion recursesubdirs createallsubdirs


[Icons]

Name: "{group}\flm"; \
    Filename: "{app}\flm.exe"; \
    WorkingDir: "{app}"; \
    IconFilename: "{app}\logo.ico"; \
    IconIndex: 0; \
    Comment: "Launch flm"

; Desktop shortcut (conditional based on user choice)
Name: "{commondesktop}\flm run"; \
    Filename: "{sys}\cmd.exe"; \
    Parameters: "/K ""{app}\flm.exe"" run llama3.2:1b"; \
    WorkingDir: "{app}"; \
    IconFilename: "{app}\logo.ico"; \
    IconIndex: 0; \
    Comment: "Launch flm with llama3.2:1b model"; \
    Tasks: desktopicon


; Desktop shortcut (conditional based on user choice)
Name: "{commondesktop}\flm serve"; \
    Filename: "{sys}\cmd.exe"; \
    Parameters: "/K ""{app}\flm.exe"" serve"; \
    WorkingDir: "{app}"; \
    IconFilename: "{app}\logo.ico"; \
    IconIndex: 0; \
    Comment: "Launch flm in serve mode"; \
    Tasks: desktopicon

[Tasks]
; Optional desktop icon task

Name: "desktopicon"; Description: "Create a desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[Code]
var
  ModelPathPage: TInputDirWizardPage;
  PortPage: TInputQueryWizardPage;
  CustomModelPath: string;
  CustomPort: string;

function DirInPath(Path: string; Dir: string): Boolean;
var
  I: Integer;
  Entry: string;
begin
  Result := False;
  Path := Path + ';';

  if (Length(Dir) > 0) and (Dir[Length(Dir)] = '\') then
    Dir := Copy(Dir, 1, Length(Dir) - 1);
  Dir := Uppercase(Dir);

  while Path <> '' do
  begin
    I := Pos(';', Path);
    if I > 0 then
    begin
      Entry := Copy(Path, 1, I - 1);
      Delete(Path, 1, I);
      Entry := Trim(Entry);
      if Entry <> '' then
      begin
        if (Length(Entry) > 0) and (Entry[Length(Entry)] = '\') then
          Entry := Copy(Entry, 1, Length(Entry) - 1);

        if Uppercase(Entry) = Dir then
        begin
          Result := True;
          Exit;
        end;
      end;
    end
    else
      Path := '';
  end;
end;

procedure RemoveDirFromPath(var Path: string; Dir: string);
var
  NewPath: string;
  I: Integer;
  Entry: string;
  CompareEntry: string;
begin
  NewPath := '';
  Path := Path + ';';

  if (Length(Dir) > 0) and (Dir[Length(Dir)] = '\') then
    Dir := Copy(Dir, 1, Length(Dir) - 1);
  Dir := Uppercase(Dir);

  while Path <> '' do
  begin
    I := Pos(';', Path);
    if I > 0 then
    begin
      Entry := Copy(Path, 1, I - 1);
      Delete(Path, 1, I);
      Entry := Trim(Entry);
      if Entry <> '' then
      begin
        CompareEntry := Entry;
        if (Length(CompareEntry) > 0) and (CompareEntry[Length(CompareEntry)] = '\') then
          CompareEntry := Copy(CompareEntry, 1, Length(CompareEntry) - 1);

        if Uppercase(CompareEntry) <> Dir then
        begin
          if NewPath <> '' then
            NewPath := NewPath + ';';
          NewPath := NewPath + Entry;
        end;
      end;
    end
    else
      Path := '';
  end;
  Path := NewPath;
end;

function GetExistingModelPath: string;
var
  ExistingPath: string;
begin
  // Check if FLM_MODEL_PATH environment variable already exists
  if RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'FLM_MODEL_PATH', ExistingPath)
  then begin
    // FLM_MODEL_PATH exists, use it as default
    Result := ExistingPath;
  end
  else begin
    // FLM_MODEL_PATH doesn't exist, use default userdocs location
    Result := GetEnv('USERPROFILE') + '\.flm';
  end;
end;

function GetExistingPort: string;
var
  ExistingPort: string;
begin
  // Check if FLM_SERVE_PORT environment variable already exists
  if RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'FLM_SERVE_PORT', ExistingPort)
  then begin
    // FLM_SERVE_PORT exists, use it as default
    Result := ExistingPort;
  end
  else begin
    // FLM_SERVE_PORT doesn't exist, use default port
    Result := '52625';
  end;
  end;

procedure InitializeWizard;
begin
  ModelPathPage := CreateInputDirPage(wpSelectDir,
    'Model Storage Location', 'Where should FLM store downloaded models?',
    'Select the folder where FLM will store downloaded models, then click Next.' + #13#10 + #13#10 +
    'Existing FLM users -- restart PC after path change, then move flm/model/.',
    True, '');
  ModelPathPage.Add('Model storage location:');
  ModelPathPage.Values[0] := GetExistingModelPath;

  PortPage := CreateInputQueryPage(wpSelectDir,
    'Server Port Configuration', 'What port should FLM use for the server?',
    'Enter the port number for FLM server (default: 52625).' + #13#10 + #13#10 +
    'Existing FLM users -- the current port will be used as default.');
  PortPage.Add('Server port:', False);
  PortPage.Values[0] := GetExistingPort;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  if CurPageID = ModelPathPage.ID then
  begin
    CustomModelPath := ModelPathPage.Values[0];
  end
  else if CurPageID = PortPage.ID then
  begin
    CustomPort := PortPage.Values[0];
  end;
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  OldPath: string;
  AppPath: string;
begin
  if CurStep = ssPostInstall then
  begin
    if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', OldPath)
    then
      OldPath := '';

    AppPath := ExpandConstant('{app}');

    if not DirInPath(OldPath, AppPath) then
    begin
      if OldPath = '' then
        OldPath := AppPath
      else if OldPath[Length(OldPath)] = ';' then
        OldPath := OldPath + AppPath
      else
        OldPath := OldPath + ';' + AppPath;

      RegWriteStringValue(HKEY_LOCAL_MACHINE,
        'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
        'Path', OldPath);
    end;
    // Always set the FLM_MODEL_PATH environment variable to user's choice
    RegWriteStringValue(HKEY_LOCAL_MACHINE,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'FLM_MODEL_PATH', CustomModelPath);

    // Always set the FLM_SERVE_PORT environment variable to user's choice
    RegWriteStringValue(HKEY_LOCAL_MACHINE,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'FLM_SERVE_PORT', CustomPort);
  end;
end;

// --- Uninstall ---
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  OldPath: string;
  AppPath: string;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // --- remove from PATH  ---
    if RegQueryStringValue(HKEY_LOCAL_MACHINE,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', OldPath)
    then
    begin
      AppPath := ExpandConstant('{app}');
      if DirInPath(OldPath, AppPath) then
      begin
        RemoveDirFromPath(OldPath, AppPath);

        RegWriteStringValue(HKEY_LOCAL_MACHINE,
          'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
          'Path', OldPath);
      end;
    end;

    // --- Delete FLM_MODEL_PATH ---
    RegDeleteValue(HKEY_LOCAL_MACHINE,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'FLM_MODEL_PATH');

    // --- Delete FLM_SERVE_PORT ---
    RegDeleteValue(HKEY_LOCAL_MACHINE,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'FLM_SERVE_PORT');
  end;
end;
