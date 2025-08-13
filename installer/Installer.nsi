; Lemonade Server Installer Script

!define /ifndef NPU_DRIVER_VERSION "32.0.203.280"

; Request user rights only (no admin)
RequestExecutionLevel user

; Define main variables
Name "Lemonade Server"
OutFile "Lemonade_Server_Installer.exe"

; Include modern UI elements
!include "MUI2.nsh"

!include FileFunc.nsh

; Include LogicLib for logging in silent mode
!include LogicLib.nsh
Var LogHandle

Var LEMONADE_SERVER_STRING
Var HYBRID_SELECTED
Var HYBRID_CLI_OPTION
Var NO_DESKTOP_SHORTCUT
Var ADD_TO_STARTUP

; Variables for CPU detection
Var cpuName
Var isCpuSupported
Var ryzenAiPos
Var seriesStartPos
Var currentChar

; Used for string manipulation
!include "StrFunc.nsh"
${StrLoc}
${StrCase}
${StrTrimNewLines}
${StrTok}

; Define a section for the installation
Section "Install Main Components" SEC01
SectionIn RO ; Read only, always installed
  DetailPrint "Main Installation Section"

  ; Once we're done downloading and installing the pip packages the size comes out to about 413 MB
  AddSize 433672

  ; Run `lemonade-server stop` and wait for it to return before continuing to ensure Lemonade has been properly stopped.
  DetailPrint "Stopping any running Lemonade Server instances..."
  
  ; Check if lemonade-server.bat exists in the installation folder
  IfFileExists "$INSTDIR\bin\lemonade-server.bat" 0 stop_complete
    ; Use the existing lemonade-server.bat to stop the server (in background)
    nsExec::Exec '"$INSTDIR\bin\lemonade-server.bat" stop'
    Pop $0
    ${If} $0 == 0
      DetailPrint "- Lemonade Server stopped successfully"
    ${Else}
      DetailPrint "- Lemonade Server stop command failed (continuing anyway)"
    ${EndIf}
    Goto stop_complete

  stop_complete:

  ; Check if directory exists before proceeding
  IfFileExists "$INSTDIR\*.*" 0 continue_install
  ; Directory exists, first check if it's in use by trying to rename it
  Rename "$INSTDIR" "$INSTDIR.tmp"
    
  ; Check if rename was successful
  IfFileExists "$INSTDIR.tmp\*.*" 0 folder_in_use
    ; Rename was successful, rename it back - directory is not in use
    Rename "$INSTDIR.tmp" "$INSTDIR"
    
    ; Now ask user if they want to remove it
    ${IfNot} ${Silent}
      MessageBox MB_YESNO "An existing $LEMONADE_SERVER_STRING installation was found at $INSTDIR.$\n$\nWould you like to remove it and continue with the installation?" IDYES remove_dir
      ; If user selects No, show exit message and quit the installer
      MessageBox MB_OK "Installation cancelled. Exiting installer..."
      Quit
    ${Else}
      Goto remove_dir
    ${EndIf}

  folder_in_use:
    ; Rename failed, folder is in use
    ${IfNot} ${Silent}
      MessageBox MB_OK "The installation folder is currently being used. To proceed, please follow these steps:$\n$\n1. Close any open files or folders from the installation directory$\n2. If Lemonade Server is running, click 'Quit' from the tray icon$\n3. Open a terminal and run: lemonade-server stop$\n4. If still running, end lemonade-server.exe and llama-server.exe in Task Manager$\n$\nIf the issue persists, try restarting your computer and run the installer again."
    ${EndIf}
    Quit

  remove_dir:
    ; Remove directory (we already know it's not in use)
    RMDir /r "$INSTDIR"
    
    ; Verify deletion was successful
    IfFileExists "$INSTDIR\*.*" 0 continue_install
      ${IfNot} ${Silent}
        MessageBox MB_OK "Unable to remove existing installation. Please close any applications using $LEMONADE_SERVER_STRING and try again."
      ${EndIf}
      Quit

  continue_install:
    ; Create fresh directory
    CreateDirectory "$INSTDIR"
    DetailPrint "*** INSTALLATION STARTED ***"

    ; Attach console to installation to enable logging
    System::Call 'kernel32::GetStdHandle(i -11)i.r0'
    StrCpy $LogHandle $0 ; Save the handle to LogHandle variable
    System::Call 'kernel32::AttachConsole(i -1)i.r1'
    ${If} $LogHandle = 0
      ${OrIf} $1 = 0
      System::Call 'kernel32::AllocConsole()'
      System::Call 'kernel32::GetStdHandle(i -11)i.r0'
      StrCpy $LogHandle $0 ; Update the LogHandle variable if the console was allocated
    ${EndIf}
    DetailPrint "- Initialized logging"

    ; Set the output path for future operations
    SetOutPath "$INSTDIR"

    DetailPrint "Starting '$LEMONADE_SERVER_STRING' Installation..."
    DetailPrint 'Configuration:'
    DetailPrint '  Install Dir: $INSTDIR'
    DetailPrint '  Minimum NPU Driver Version: ${NPU_DRIVER_VERSION}'
    DetailPrint '-------------------------------------------'

    # Pack lemonade repo into the installer
    # Exclude hidden files (like .git, .gitignore) and the installation folder itself
    File /r /x nsis.exe /x installer /x .* /x *.pyc /x docs /x examples /x utilities ..\*.* lemonade-server.bat add_to_path.py lemonade_notification.vbs lemonade_server.vbs

    # Create bin directory and move lemonade-server.bat there
    CreateDirectory "$INSTDIR\bin"
    Rename "$INSTDIR\lemonade-server.bat" "$INSTDIR\bin\lemonade-server.bat"
    Rename "$INSTDIR\lemonade_notification.vbs" "$INSTDIR\bin\lemonade_notification.vbs"
    Rename "$INSTDIR\lemonade_server.vbs" "$INSTDIR\bin\lemonade_server.vbs"

    DetailPrint "- Packaged repo"

    DetailPrint "Set up Python"
    CreateDirectory "$INSTDIR\python"
    ExecWait 'curl -s -o "$INSTDIR\python\python.zip" "https://www.python.org/ftp/python/3.10.9/python-3.10.9-embed-amd64.zip"'
    ExecWait 'tar -xf "$INSTDIR\python\python.zip" -C "$INSTDIR\python"'
    ExecWait 'curl -sSL https://bootstrap.pypa.io/get-pip.py -o get-pip.py'
    ExecWait '"$INSTDIR\python\python.exe" get-pip.py --no-warn-script-location'
    
    FileOpen $2 "$INSTDIR\python\python310._pth" a
    FileSeek $2 0 END
    FileWrite $2 "$\r$\nLib$\r$\n"
    FileWrite $2 "$\r$\nLib\site-packages$\r$\n"
    FileWrite $2 "$\r$\nLib\site-packages\win32$\r$\n"
    FileWrite $2 "$\r$\nLib\site-packages\win32\lib$\r$\n"
    FileWrite $2 "$\r$\nLib\site-packages\Pythonwin$\r$\n"
    FileClose $2

    DetailPrint "-------------------------"
    DetailPrint "- Lemonade Installation -"
    DetailPrint "-------------------------" 




    DetailPrint "- Installing $LEMONADE_SERVER_STRING..."
    
    ; Always install base CPU version first to ensure lemonade-server-dev.exe is available
    ExecWait '"$INSTDIR\python\python.exe" -m pip install "$INSTDIR"[oga-cpu] --no-warn-script-location' $8
    DetailPrint "- Base lemonade installation return code: $8"
    
    ; Check if base installation was successful
    StrCmp $8 0 base_install_success base_install_failed
    
    base_install_success:
      DetailPrint "- Base $LEMONADE_SERVER_STRING installation successful"
      
      ; If hybrid mode is selected, upgrade to hybrid
      ${If} $HYBRID_SELECTED == "true"
        DetailPrint "- Upgrading to hybrid mode..."
                 ExecWait '"$INSTDIR\python\python.exe" -m pip install "$INSTDIR"[oga-ryzenai] --extra-index-url=https://pypi.amd.com/simple --no-warn-script-location' $8
        DetailPrint "- Hybrid upgrade return code: $8"
        
        ; Check if hybrid upgrade was successful
        StrCmp $8 0 hybrid_upgrade_success hybrid_upgrade_failed
        
        hybrid_upgrade_success:
          DetailPrint "- Hybrid mode upgrade successful"
          Goto install_success
          
        hybrid_upgrade_failed:
          DetailPrint "- Hybrid mode upgrade failed"
          Goto install_failed
      ${Else}
        DetailPrint "- CPU-only installation completed"
        Goto install_success
      ${EndIf}
      
    base_install_failed:
      DetailPrint "- Base $LEMONADE_SERVER_STRING installation failed"
      Goto install_failed

    install_success:
      DetailPrint "- $LEMONADE_SERVER_STRING installation successful"

      DetailPrint "*** INSTALLATION COMPLETED ***"
      # Create a shortcut inside $INSTDIR
      CreateShortcut "$INSTDIR\lemonade-server.lnk" "$INSTDIR\bin\lemonade_server.vbs" "" "$INSTDIR\src\lemonade\tools\server\static\favicon.ico"

      ; Add bin folder to user PATH
      DetailPrint "- Adding bin directory to user PATH..."
      
      ; Add to user path without replication
      ; If the folder is already on path, we move it to the top
      ExecWait '"$INSTDIR\python\python.exe" add_to_path.py "$INSTDIR\bin"' $8
      DetailPrint "- $LEMONADE_SERVER_STRING install return code: $8"

      ; Check if path setting was successful
      ; If the exit code is 0, move to the next line (+1), otherwise install_failed
      StrCmp $8 0 +1 install_failed

      ; Notify Windows that environment variables have changed
      SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000

      Goto end

    install_failed:
      DetailPrint "- $LEMONADE_SERVER_STRING installation failed"
      ${IfNot} ${Silent}
        MessageBox MB_OK "ERROR: $LEMONADE_SERVER_STRING package failed to install using pip. Installation will be aborted."
      ${EndIf}
      Quit

    end:
SectionEnd

Section "Ryzen AI SW: NPU and Hybrid support" HybridSec
  SectionIn 1
  AddSize 132188
  StrCpy $HYBRID_SELECTED "true"
SectionEnd

SubSection /e "Selected Models" ModelsSec
  Section "Qwen2.5-0.5B-Instruct-CPU" Qwen05Sec
    SectionIn RO ; Read only, always installed
    AddSize 833871  ;
  SectionEnd

  Section "-Download Models" DownloadModels
    ; Always download the Qwen2.5-0.5B model
    nsExec::ExecToLog '$INSTDIR\python\Scripts\lemonade-server-dev pull Qwen2.5-0.5B-Instruct-CPU'
  SectionEnd

SubSectionEnd

Section "-Add Desktop Shortcut" ShortcutSec  
  ${If} $NO_DESKTOP_SHORTCUT != "true"
    CreateShortcut "$DESKTOP\lemonade-server.lnk" "$INSTDIR\bin\lemonade_server.vbs" "" "$INSTDIR\src\lemonade\tools\server\static\favicon.ico"
  ${EndIf}
SectionEnd

Function RunServer
  ExecShell "open" "$INSTDIR\LEMONADE-SERVER.lnk"
FunctionEnd

Function AddToStartup
  ; Delete existing shortcut if it exists
  Delete "$SMSTARTUP\lemonade-server.lnk"
  ; Create shortcut in the startup folder
  CreateShortcut "$SMSTARTUP\lemonade-server.lnk" "$INSTDIR\bin\lemonade_server.vbs" "" "$INSTDIR\src\lemonade\tools\server\static\favicon.ico"
FunctionEnd

; Define constants for better readability
!define ICON_FILE "..\src\lemonade\tools\server\static\favicon.ico"

; Finish Page settings
!define MUI_TEXT_FINISH_INFO_TITLE "$LEMONADE_SERVER_STRING installed successfully!"
!define MUI_TEXT_FINISH_INFO_TEXT "A shortcut has been added to your Desktop. What would you like to do next?"
!define MUI_WELCOMEFINISHPAGE_BITMAP "installer_banner.bmp"

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION RunServer
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_RUN_TEXT "Run Lemonade Server"

!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Run at Startup"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION AddToStartup

Function .onSelChange
    ; Check hybrid selection status
    StrCpy $HYBRID_SELECTED "false"
    SectionGetFlags ${HybridSec} $0
    IntOp $0 $0 & ${SF_SELECTED}
    StrCmp $0 ${SF_SELECTED} 0 hybrid_disabled
    StrCpy $HYBRID_SELECTED "true"
    Goto end
    
hybrid_disabled:
    StrCpy $HYBRID_SELECTED "false"

end:
FunctionEnd

Function SkipLicense
  ${IfNot} ${SectionIsSelected} ${HybridSec}
    abort  ;skip AMD license if hybrid was not enabled
  ${EndIf}
FunctionEnd


; MUI Settings
!insertmacro MUI_PAGE_WELCOME
!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_COMPONENTS

!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipLicense
!insertmacro MUI_PAGE_LICENSE "AMD_LICENSE"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

!define MUI_PAGE_CUSTOMFUNCTION_SHOW .onSelChange




; Set the installer icon
Icon ${ICON_FILE}

; Language settings
LangString MUI_TEXT_WELCOME_INFO_TITLE "${LANG_ENGLISH}" "Welcome to the $LEMONADE_SERVER_STRING Installer"
LangString MUI_TEXT_WELCOME_INFO_TEXT "${LANG_ENGLISH}" "This wizard will install $LEMONADE_SERVER_STRING on your computer."
LangString MUI_TEXT_DIRECTORY_TITLE "${LANG_ENGLISH}" "Select Installation Directory"
LangString MUI_TEXT_INSTALLING_TITLE "${LANG_ENGLISH}" "Installing $LEMONADE_SERVER_STRING"
LangString MUI_TEXT_FINISH_TITLE "${LANG_ENGLISH}" "Installation Complete"
LangString MUI_TEXT_FINISH_SUBTITLE "${LANG_ENGLISH}" "Thank you for installing $LEMONADE_SERVER_STRING!"
LangString MUI_TEXT_ABORT_TITLE "${LANG_ENGLISH}" "Installation Aborted"
LangString MUI_TEXT_ABORT_SUBTITLE "${LANG_ENGLISH}" "Installation has been aborted."
LangString MUI_BUTTONTEXT_FINISH "${LANG_ENGLISH}" "Finish"
LangString MUI_TEXT_LICENSE_TITLE ${LANG_ENGLISH} "AMD License Agreement"
LangString MUI_TEXT_LICENSE_SUBTITLE ${LANG_ENGLISH} "Please review the license terms before installing AMD Ryzen AI SW with NPU and Hybrid Support."
LangString DESC_SEC01 ${LANG_ENGLISH} "The minimum set of dependencies for a lemonade server that runs LLMs on CPU (includes Python)."
LangString DESC_HybridSec ${LANG_ENGLISH} "Add support for running LLMs on Ryzen AI SW with NPU and Hybrid Support. Only available on Ryzen AI 300-series processors."
LangString DESC_ModelsSec ${LANG_ENGLISH} "Default model for Lemonade Server"
LangString DESC_Qwen05Sec ${LANG_ENGLISH} "Qwen2.5-0.5B-Instruct-CPU model (ONNX format). This lightweight model helps you quickly get started with Lemonade Server."

; Insert the description macros
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC01} $(DESC_SEC01)
  !insertmacro MUI_DESCRIPTION_TEXT ${HybridSec} $(DESC_HybridSec)
  !insertmacro MUI_DESCRIPTION_TEXT ${ModelsSec} $(DESC_ModelsSec)
  !insertmacro MUI_DESCRIPTION_TEXT ${Qwen05Sec} $(DESC_Qwen05Sec)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  StrCpy $LEMONADE_SERVER_STRING "Lemonade Server"
  StrCpy $HYBRID_SELECTED "true"
  StrCpy $NO_DESKTOP_SHORTCUT "false"
  StrCpy $ADD_TO_STARTUP "false"

  ; Set the install directory, allowing /D override from CLI install
  ${If} $InstDir != ""
    ; /D was used
  ${Else}
    ; Use the default
    StrCpy $InstDir "$LOCALAPPDATA\lemonade_server"
  ${EndIf}

  ; Check if NoDesktopShortcut parameter was used
  ${GetParameters} $CMDLINE
  ${GetOptions} $CMDLINE "/NoDesktopShortcut" $R0
  ${If} $R0 != ""
    StrCpy $NO_DESKTOP_SHORTCUT "true"
  ${EndIf}

  ; Check if AddToStartup parameter was used
  ${GetOptions} $CMDLINE "/AddToStartup" $R0
  ${If} $R0 != ""
    StrCpy $ADD_TO_STARTUP "true"
    Call AddToStartup
  ${EndIf}

  ; Check CPU name to determine if Hybrid section should be enabled
  DetailPrint "Checking CPU model..."
  
  ; Use PowerShell to get CPU name (RemoteSigned is less suspicious than Bypass)
  nsExec::ExecToStack 'powershell -NoProfile -ExecutionPolicy RemoteSigned -Command "Get-WmiObject -Class Win32_Processor | Select-Object -ExpandProperty Name"'
  Pop $0 ; Return value
  Pop $cpuName ; Output (CPU name)
  
  ; Check if WMI call was successful
  ${If} $0 != "0"
    DetailPrint "WMI CPU detection failed (return code: $0). Continuing with default behavior."
    StrCpy $cpuName "Unknown CPU"
  ${Else}
    DetailPrint "Detected CPU: $cpuName"
  ${EndIf}
  
  ; Check if CPU name contains "Ryzen AI" and a 3-digit number starting with 3
  StrCpy $isCpuSupported "false" ; Initialize CPU allowed flag to false
  
  ; Convert cpuName to lowercase
  ${StrCase} $cpuName "$cpuName" "L"
  ${StrLoc} $ryzenAiPos $cpuName "ryzen ai" ">"
  ${If} $ryzenAiPos != ""
    ; Found "Ryzen AI", now look for 3xx series
    ${StrLoc} $seriesStartPos $cpuName " 3" ">"
    ${If} $seriesStartPos != ""
      ; Check if the character after "3" is a digit (first digit of model number)
      StrCpy $currentChar $cpuName 1 $seriesStartPos+2
      ${If} $currentChar >= "0"
        ${AndIf} $currentChar <= "9"
        ; Check if the character after that is also a digit (second digit of model number)
        StrCpy $currentChar $cpuName 1 $seriesStartPos+3
        ${If} $currentChar >= "0"
          ${AndIf} $currentChar <= "9"
          ; Check if the character after the third digit is a space or end of string
          StrCpy $currentChar $cpuName 1 $seriesStartPos+4
          ${If} $currentChar == " "
            ${OrIf} $currentChar == ""
            ; Found a complete 3-digit number starting with 3
            StrCpy $isCpuSupported "true"
            DetailPrint "Detected Ryzen AI 3xx series processor"
          ${EndIf}
        ${EndIf}
      ${EndIf}
    ${EndIf}
  ${EndIf}
  
  DetailPrint "CPU is compatible with Ryzen AI hybrid software: $isCpuSupported"
  
  ; Check if CPU is in the allowed models list
  ; Only disable if we successfully detected an incompatible CPU
  ${If} $isCpuSupported != "true"
    ${AndIf} $cpuName != "Unknown CPU"
    ; Disable Hybrid section if CPU is not in allowed list
    SectionGetFlags ${HybridSec} $0
    IntOp $0 $0 & ${SECTION_OFF}    ; Turn off selection
    IntOp $0 $0 | ${SF_RO}          ; Make it read-only (can't be selected)
    SectionSetFlags ${HybridSec} $0
    StrCpy $HYBRID_SELECTED "false"
  ${ElseIf} $cpuName == "Unknown CPU"
    ; If CPU detection failed, allow user to choose but warn them
    DetailPrint "CPU detection failed - user can choose hybrid mode at their own risk"
  ${EndIf}

  ; Disable hybrid mode by default in silent mode
  ; Use /Extras="hybrid" option to enable it
  ${If} ${Silent}
    
    ${GetParameters} $CMDLINE
    ${GetOptions} $CMDLINE "/Extras=" $HYBRID_CLI_OPTION

    ${IfNot} $HYBRID_CLI_OPTION == "hybrid"
      SectionSetFlags ${HybridSec} 0
      StrCpy $HYBRID_SELECTED "false"
    ${ElseIf} $isCpuSupported != "true"
      ; Don't allow hybrid mode if CPU is not in allowed list, even if specified in command line
      SectionSetFlags ${HybridSec} 0
      StrCpy $HYBRID_SELECTED "false"
    ${EndIf}
  ${EndIf}

  ; Call onSelChange to ensure initial hybrid selection state is correct
  Call .onSelChange

FunctionEnd

; This file was originally licensed under Apache 2.0. It has been modified.
; Modifications Copyright (c) 2025 AMD