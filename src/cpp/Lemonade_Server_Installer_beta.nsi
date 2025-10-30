; Lemonade Server Beta Installer Script (C++ Version)

; Request user rights only (no admin)
RequestExecutionLevel user

; Compression settings - use LZMA with solid compression for better compression ratio
SetCompressor /SOLID lzma
SetCompressorDictSize 32
SetDatablockOptimize on

; Define main variables
Name "Lemonade Server Beta"
OutFile "Lemonade_Server_Installer_beta.exe"

; Include modern UI elements
!include "MUI2.nsh"
!include FileFunc.nsh
!include LogicLib.nsh

; Define constants
!define PRODUCT_NAME "Lemonade Server Beta"
!define PRODUCT_VERSION "1.0.0"
!define PRODUCT_PUBLISHER "AMD"
!define PRODUCT_WEB_SITE "https://lemonade-server.ai"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"

Var NO_DESKTOP_SHORTCUT
Var ADD_TO_STARTUP

; Define a section for the installation
Section "Install Lemonade Server Beta" SEC01
SectionIn RO ; Read only, always installed
  DetailPrint "Installing Lemonade Server Beta..."

  ; Stop any running instances gracefully
  DetailPrint "Checking for running Lemonade Server instances..."
  
  ; Try to find and close the window gracefully instead of force-killing
  FindWindow $0 "" "Lemonade Server Beta"
  ${If} $0 != 0
    DetailPrint "Requesting Lemonade Server to close..."
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 2000  ; Give it time to close gracefully
  ${EndIf}

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
      MessageBox MB_YESNO "An existing Lemonade Server Beta installation was found at $INSTDIR.$\n$\nWould you like to remove it and continue with the installation?" IDYES remove_dir
      ; If user selects No, show exit message and quit the installer
      MessageBox MB_OK "Installation cancelled. Exiting installer..."
      Quit
    ${Else}
      Goto remove_dir
    ${EndIf}

  folder_in_use:
    ; Rename failed, folder is in use
    ${IfNot} ${Silent}
      MessageBox MB_OK "The installation folder is currently being used. To proceed, please follow these steps:$\n$\n1. Close any open files or folders from the installation directory$\n2. If Lemonade Server is running, right-click the tray icon and click 'Quit'$\n3. End lemonade-server-beta.exe and lemonade-router.exe in Task Manager$\n$\nIf the issue persists, try restarting your computer and run the installer again."
    ${EndIf}
    Quit

  remove_dir:
    ; Remove directory (we already know it's not in use)
    RMDir /r "$INSTDIR"
    
    ; Verify deletion was successful
    IfFileExists "$INSTDIR\*.*" 0 continue_install
      ${IfNot} ${Silent}
        MessageBox MB_OK "Unable to remove existing installation. Please close any applications using Lemonade Server Beta and try again."
      ${EndIf}
      Quit

  continue_install:
    ; Create fresh directory structure
    CreateDirectory "$INSTDIR"
    CreateDirectory "$INSTDIR\bin"
    CreateDirectory "$INSTDIR\resources"
    
    DetailPrint "*** INSTALLATION STARTED ***"
    DetailPrint 'Configuration:'
    DetailPrint '  Install Dir: $INSTDIR'
    DetailPrint '-------------------------------------------'

    ; Set the output path for future operations
    SetOutPath "$INSTDIR\bin"

    ; Copy the executables from the build directory
    DetailPrint "Installing application files..."
    File "build\Release\lemonade-server-beta.exe"
    DetailPrint "- Installed Lemonade Server tray application"
    
    File "build\Release\lemonade-router.exe"
    DetailPrint "- Installed Lemonade AI Server engine"
    
    File "build\Release\zstd.dll"
    DetailPrint "- Installed required library: zstd.dll"

    ; Copy resources (icon, etc.) to bin directory so lemonade-router.exe can find them
    ; The server looks for resources relative to the executable directory
    DetailPrint "Installing application resources..."
    File /r "build\Release\resources"
    DetailPrint "- Installed web UI and configuration files"

    ; Add bin folder to user PATH using registry directly
    DetailPrint "Configuring environment..."
    
    ; Read current PATH from registry
    ReadRegStr $0 HKCU "Environment" "Path"
    
    ; Check if our path is already in there using simple string search
    Push "$0"
    Push "$INSTDIR\bin"
    Call StrStr
    Pop $1
    
    ; If not found ($1 is empty), add it
    StrCmp $1 "" 0 skip_path_add
    
    ; Append our path
    StrCpy $0 "$INSTDIR\bin;$0"
    
    ; Write back to registry
    WriteRegExpandStr HKCU "Environment" "Path" $0
    DetailPrint "- Added installation directory to user PATH"
    Goto path_done
    
    skip_path_add:
    DetailPrint "- Installation directory already in PATH"
    
    path_done:
    ; Notify Windows that environment variables have changed
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000

    ; Create Start Menu shortcuts
    DetailPrint "Creating shortcuts..."
    CreateDirectory "$SMPROGRAMS\Lemonade Server Beta"
    CreateShortcut "$SMPROGRAMS\Lemonade Server Beta\Lemonade Server Beta.lnk" "$INSTDIR\bin\lemonade-server-beta.exe" "" "$INSTDIR\bin\resources\static\favicon.ico" 0
    CreateShortcut "$SMPROGRAMS\Lemonade Server Beta\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
    DetailPrint "- Created Start Menu shortcuts"

    ; Write uninstaller
    DetailPrint "Registering uninstaller..."
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    
    ; Add uninstall information to Add/Remove Programs
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bin\resources\static\favicon.ico"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
    DetailPrint "- Registered with Windows Programs and Features"

    DetailPrint "-------------------------------------------"
    DetailPrint "*** INSTALLATION COMPLETED SUCCESSFULLY ***"
SectionEnd

Section "-Add Desktop Shortcut" ShortcutSec  
  ${If} $NO_DESKTOP_SHORTCUT != "true"
    CreateShortcut "$DESKTOP\Lemonade Server Beta.lnk" "$INSTDIR\bin\lemonade-server-beta.exe" "" "$INSTDIR\bin\resources\static\favicon.ico" 0
  ${EndIf}
SectionEnd

Function RunServer
  Exec '"$INSTDIR\bin\lemonade-server-beta.exe"'
FunctionEnd

Function AddToStartup
  ; Delete existing shortcut if it exists
  Delete "$SMSTARTUP\Lemonade Server Beta.lnk"
  ; Create shortcut in the startup folder
  CreateShortcut "$SMSTARTUP\Lemonade Server Beta.lnk" "$INSTDIR\bin\lemonade-server-beta.exe" "" "$INSTDIR\bin\resources\static\favicon.ico" 0
FunctionEnd

; Finish Page settings
!define MUI_TEXT_FINISH_INFO_TITLE "${PRODUCT_NAME} installed successfully!"
!define MUI_TEXT_FINISH_INFO_TEXT "A shortcut has been added to your Desktop. What would you like to do next?"
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\..\installer\installer_banner.bmp"

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION RunServer
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_RUN_TEXT "Run Lemonade Server"

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

; Set the installer and uninstaller icons
!define ICON_FILE "..\..\src\lemonade\tools\server\static\favicon.ico"
Icon ${ICON_FILE}
UninstallIcon ${ICON_FILE}

; Language settings
LangString MUI_TEXT_WELCOME_INFO_TITLE "${LANG_ENGLISH}" "Welcome to the ${PRODUCT_NAME} Installer"
LangString MUI_TEXT_WELCOME_INFO_TEXT "${LANG_ENGLISH}" "This wizard will install ${PRODUCT_NAME} on your computer."
LangString MUI_TEXT_DIRECTORY_TITLE "${LANG_ENGLISH}" "Select Installation Directory"
LangString MUI_TEXT_INSTALLING_TITLE "${LANG_ENGLISH}" "Installing Lemonade Server Beta"
LangString MUI_TEXT_FINISH_TITLE "${LANG_ENGLISH}" "Installation Complete"
LangString MUI_BUTTONTEXT_FINISH "${LANG_ENGLISH}" "Finish"

; Helper function to search for a substring in a string
; Usage: Push haystack, Push needle, Call StrStr, Pop result
Function StrStr
  Exch $R1 ; needle
  Exch
  Exch $R2 ; haystack
  Push $R3
  Push $R4
  Push $R5
  StrLen $R3 $R1
  StrCpy $R4 0
  loop:
    StrCpy $R5 $R2 $R3 $R4
    StrCmp $R5 $R1 done
    StrCmp $R5 "" done
    IntOp $R4 $R4 + 1
    Goto loop
  done:
    StrCpy $R1 $R2 "" $R4
    Pop $R5
    Pop $R4
    Pop $R3
    Pop $R2
    Exch $R1
FunctionEnd

Function .onInit
  StrCpy $NO_DESKTOP_SHORTCUT "false"
  StrCpy $ADD_TO_STARTUP "false"

  ; Set the install directory, allowing /D override from CLI install
  ${If} $InstDir != ""
    ; /D was used
  ${Else}
    ; Use the default
    StrCpy $InstDir "$LOCALAPPDATA\lemonade_server_beta"
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

; Helper function to replace a substring (uninstaller version)
Function un.StrRep
  Exch $R4 ; replacement
  Exch
  Exch $R3 ; string to replace
  Exch 2
  Exch $R1 ; input string
  Push $R2
  Push $R5
  Push $R6
  Push $R7
  Push $R8
  
  StrCpy $R2 ""
  StrLen $R5 $R3
  ${If} $R5 == 0
    StrCpy $R1 $R1
    Goto done
  ${EndIf}
  
  loop:
    StrCpy $R6 $R1 $R5
    StrCmp $R6 $R3 found
    StrCpy $R6 $R1 1
    StrCpy $R2 $R2$R6
    StrCpy $R1 $R1 "" 1
    StrCmp $R1 "" done loop
    
  found:
    StrCpy $R2 $R2$R4
    StrCpy $R1 $R1 "" $R5
    Goto loop
    
  done:
    StrCpy $R1 $R2$R1
    Pop $R8
    Pop $R7
    Pop $R6
    Pop $R5
    Pop $R2
    Exch 2
    Pop $R3
    Pop $R4
    Exch $R1
FunctionEnd

; Uninstaller Section
Section "Uninstall"
  ; Stop any running instances gracefully
  FindWindow $0 "" "Lemonade Server Beta"
  ${If} $0 != 0
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 2000  ; Give it time to close gracefully
  ${EndIf}

  ; Remove files
  Delete "$INSTDIR\bin\lemonade-server-beta.exe"
  Delete "$INSTDIR\bin\lemonade-router.exe"
  Delete "$INSTDIR\bin\zstd.dll"
  Delete "$INSTDIR\Uninstall.exe"

  ; Remove directories
  RMDir /r "$INSTDIR\bin\resources"
  RMDir "$INSTDIR\bin"
  RMDir "$INSTDIR"

  ; Remove shortcuts
  Delete "$DESKTOP\Lemonade Server Beta.lnk"
  Delete "$SMSTARTUP\Lemonade Server Beta.lnk"
  RMDir /r "$SMPROGRAMS\Lemonade Server Beta"

  ; Remove from PATH using registry
  ReadRegStr $0 HKCU "Environment" "Path"
  
  ; Simple string replace - remove our path
  Push "$0"
  Push "$INSTDIR\bin;"
  Push ""
  Call un.StrRep
  Pop $0
  
  Push "$0"
  Push ";$INSTDIR\bin"
  Push ""
  Call un.StrRep
  Pop $0
  
  Push "$0"
  Push "$INSTDIR\bin"
  Push ""
  Call un.StrRep
  Pop $0
  
  WriteRegExpandStr HKCU "Environment" "Path" $0
  
  ; Remove from Add/Remove Programs
  DeleteRegKey HKCU "${UNINST_KEY}"
  
  ; Notify Windows that environment variables have changed
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd

