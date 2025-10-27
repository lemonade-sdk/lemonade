; RyzenAI Server Installer Script

; Request user rights only (no admin)
RequestExecutionLevel user

; Define main variables
Name "RyzenAI Server"
OutFile "RyzenAI_Server_Installer.exe"

; Include modern UI elements
!include "MUI2.nsh"
!include FileFunc.nsh
!include LogicLib.nsh

Var NO_DESKTOP_SHORTCUT
Var ADD_TO_STARTUP

; Define a section for the installation
Section "Install RyzenAI Server" SEC01
SectionIn RO ; Read only, always installed
  DetailPrint "Installing RyzenAI Server..."

  ; Stop any running instances
  DetailPrint "Stopping any running RyzenAI Server instances..."
  nsExec::Exec 'taskkill /F /IM ryzenai-serve.exe'
  Sleep 1000  ; Wait a second for process to terminate

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
      MessageBox MB_YESNO "An existing RyzenAI Server installation was found at $INSTDIR.$\n$\nWould you like to remove it and continue with the installation?" IDYES remove_dir
      ; If user selects No, show exit message and quit the installer
      MessageBox MB_OK "Installation cancelled. Exiting installer..."
      Quit
    ${Else}
      Goto remove_dir
    ${EndIf}

  folder_in_use:
    ; Rename failed, folder is in use
    ${IfNot} ${Silent}
      MessageBox MB_OK "The installation folder is currently being used. To proceed, please follow these steps:$\n$\n1. Close any open files or folders from the installation directory$\n2. End ryzenai-serve.exe in Task Manager$\n$\nIf the issue persists, try restarting your computer and run the installer again."
    ${EndIf}
    Quit

  remove_dir:
    ; Remove directory (we already know it's not in use)
    RMDir /r "$INSTDIR"
    
    ; Verify deletion was successful
    IfFileExists "$INSTDIR\*.*" 0 continue_install
      ${IfNot} ${Silent}
        MessageBox MB_OK "Unable to remove existing installation. Please close any applications using RyzenAI Server and try again."
      ${EndIf}
      Quit

  continue_install:
    ; Create fresh directory
    CreateDirectory "$INSTDIR"
    CreateDirectory "$INSTDIR\bin"
    
    DetailPrint "*** INSTALLATION STARTED ***"
    DetailPrint 'Configuration:'
    DetailPrint '  Install Dir: $INSTDIR'
    DetailPrint '-------------------------------------------'

    ; Set the output path for future operations
    SetOutPath "$INSTDIR\bin"

    ; Copy the executable from the build directory
    File "build\bin\Release\ryzenai-serve.exe"
    
    DetailPrint "- Copied ryzenai-serve.exe"

    ; Add bin folder to user PATH
    DetailPrint "- Adding bin directory to user PATH..."
    nsExec::ExecToLog 'powershell -NoProfile -Command "$$path = [Environment]::GetEnvironmentVariable(''Path'', ''User''); if ($$path -notlike ''*$INSTDIR\bin*'') { [Environment]::SetEnvironmentVariable(''Path'', ''$INSTDIR\bin;'' + $$path, ''User'') }"'
    
    ; Notify Windows that environment variables have changed
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000

    ; Create Start Menu shortcut
    CreateDirectory "$SMPROGRAMS\RyzenAI Server"
    CreateShortcut "$SMPROGRAMS\RyzenAI Server\RyzenAI Server.lnk" "$INSTDIR\bin\ryzenai-serve.exe" "" "$INSTDIR\bin\ryzenai-serve.exe" 0
    CreateShortcut "$SMPROGRAMS\RyzenAI Server\Uninstall.lnk" "$INSTDIR\Uninstall.exe"

    ; Write uninstaller
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    DetailPrint "*** INSTALLATION COMPLETED ***"
SectionEnd

Section "-Add Desktop Shortcut" ShortcutSec  
  ${If} $NO_DESKTOP_SHORTCUT != "true"
    CreateShortcut "$DESKTOP\RyzenAI Server.lnk" "$INSTDIR\bin\ryzenai-serve.exe" "" "$INSTDIR\bin\ryzenai-serve.exe" 0
  ${EndIf}
SectionEnd

Function RunServer
  Exec '"$INSTDIR\bin\ryzenai-serve.exe"'
FunctionEnd

Function AddToStartup
  ; Delete existing shortcut if it exists
  Delete "$SMSTARTUP\RyzenAI Server.lnk"
  ; Create shortcut in the startup folder
  CreateShortcut "$SMSTARTUP\RyzenAI Server.lnk" "$INSTDIR\bin\ryzenai-serve.exe" "" "$INSTDIR\bin\ryzenai-serve.exe" 0
FunctionEnd

; Finish Page settings
!define MUI_TEXT_FINISH_INFO_TITLE "RyzenAI Server installed successfully!"
!define MUI_TEXT_FINISH_INFO_TEXT "RyzenAI Server has been installed. What would you like to do next?"

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION RunServer
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_RUN_TEXT "Run RyzenAI Server"

!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Run at Startup"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION AddToStartup

; MUI Settings
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

; Language settings
LangString MUI_TEXT_WELCOME_INFO_TITLE "${LANG_ENGLISH}" "Welcome to the RyzenAI Server Installer"
LangString MUI_TEXT_WELCOME_INFO_TEXT "${LANG_ENGLISH}" "This wizard will install RyzenAI Server on your computer.$\n$\nRyzenAI Server provides an OpenAI-compatible API for running LLMs on AMD Ryzen AI processors."
LangString MUI_TEXT_DIRECTORY_TITLE "${LANG_ENGLISH}" "Select Installation Directory"
LangString MUI_TEXT_INSTALLING_TITLE "${LANG_ENGLISH}" "Installing RyzenAI Server"
LangString MUI_TEXT_FINISH_TITLE "${LANG_ENGLISH}" "Installation Complete"
LangString MUI_BUTTONTEXT_FINISH "${LANG_ENGLISH}" "Finish"

Function .onInit
  StrCpy $NO_DESKTOP_SHORTCUT "false"
  StrCpy $ADD_TO_STARTUP "false"

  ; Set the install directory, allowing /D override from CLI install
  ${If} $InstDir != ""
    ; /D was used
  ${Else}
    ; Use the default
    StrCpy $InstDir "$LOCALAPPDATA\ryzenai_serve"
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
FunctionEnd

; Uninstaller Section
Section "Uninstall"
  ; Stop any running instances
  nsExec::Exec 'taskkill /F /IM ryzenai-serve.exe'
  Sleep 1000

  ; Remove files
  Delete "$INSTDIR\bin\ryzenai-serve.exe"
  Delete "$INSTDIR\Uninstall.exe"

  ; Remove directories
  RMDir "$INSTDIR\bin"
  RMDir "$INSTDIR"

  ; Remove shortcuts
  Delete "$DESKTOP\RyzenAI Server.lnk"
  Delete "$SMSTARTUP\RyzenAI Server.lnk"
  RMDir /r "$SMPROGRAMS\RyzenAI Server"

  ; Remove from PATH
  nsExec::ExecToLog 'powershell -NoProfile -Command "$$path = [Environment]::GetEnvironmentVariable(''Path'', ''User''); $$newPath = ($$path -split '';'' | Where-Object { $$_ -notlike ''*ryzenai_serve*'' }) -join '';''; [Environment]::SetEnvironmentVariable(''Path'', $$newPath, ''User'')"'
  
  ; Notify Windows that environment variables have changed
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd

