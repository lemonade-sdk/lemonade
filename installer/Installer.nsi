; Lemonade Server Installer Script

!define /ifndef NPU_DRIVER_VERSION "32.0.203.237"

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
Var LEMONADE_CONDA_ENV
Var HYBRID_SELECTED
Var HYBRID_CLI_OPTION

; Define a section for the installation
Section "Install Main Components" SEC01
SectionIn RO ; Read only, always installed
  DetailPrint "------------------------"
  DetailPrint "- Installation Section -"
  DetailPrint "------------------------"

  ; Once we're done downloading and installing the pip packages the size comes out to about 2GB
  AddSize 2097152

  ; Check if directory exists before proceeding
  IfFileExists "$INSTDIR\*.*" 0 continue_install
    ${IfNot} ${Silent}
      MessageBox MB_YESNO "An existing $LEMONADE_SERVER_STRING installation was found at $INSTDIR.$\n$\nWould you like to remove it and continue with the installation?" IDYES remove_dir
      ; If user selects No, show exit message and quit the installer
      MessageBox MB_OK "Installation cancelled. Exiting installer..."
      Quit
    ${Else}
      Goto remove_dir
    ${EndIf}

  remove_dir:
    ; Try to remove directory and verify it was successful

    ; Attempt conda remove of the env, to help speed things up
    ExecWait 'conda env remove -yp "$INSTDIR\$LEMONADE_CONDA_ENV"'
    
    ; Delete all remaining files
    RMDir /r "$INSTDIR"
    
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

    # Pack turnkeyml repo into the installer
    # Exclude hidden files (like .git, .gitignore) and the installation folder itself
    File /r /x nsis.exe /x installer /x .* /x *.pyc /x docs /x examples /x utilities ..\*.* run_server.bat

    DetailPrint "- Packaged repo"

    ; Check if conda is available
    ExecWait 'where conda' $2
    DetailPrint "- Checked if conda is available"

    ; If conda is not found, show a message
    ; Otherwise, continue with the installation
    StrCmp $2 "0" create_env conda_not_available

    conda_not_available:
      DetailPrint "- Conda not installed."
      ${IfNot} ${Silent}
        MessageBox MB_YESNO "Conda is not installed. Would you like to install Miniconda?" IDYES install_miniconda IDNO exit_installer
      ${Else}
        Goto install_miniconda
      ${EndIf}

    exit_installer:
      DetailPrint "- Something went wrong. Exiting installer"
      Quit

    install_miniconda:
      DetailPrint "-------------"
      DetailPrint "- Miniconda -"
      DetailPrint "-------------"
      DetailPrint "- Downloading Miniconda installer..."
      ExecWait 'curl -s -o "$TEMP\Miniconda3-latest-Windows-x86_64.exe" "https://repo.anaconda.com/miniconda/Miniconda3-latest-Windows-x86_64.exe"'

      ; Install Miniconda silently
      ExecWait '"$TEMP\Miniconda3-latest-Windows-x86_64.exe" /InstallationType=JustMe /AddToPath=1 /RegisterPython=0 /S /D=$PROFILE\miniconda3' $2
      ; Check if Miniconda installation was successful
      ${If} $2 == 0
        DetailPrint "- Miniconda installation successful"
        ${IfNot} ${Silent}
          MessageBox MB_OK "Miniconda has been successfully installed."
        ${EndIf}

        StrCpy $R1 "$PROFILE\miniconda3\Scripts\conda.exe"
        Goto create_env

      ${Else}
        DetailPrint "- Miniconda installation failed"
        ${IfNot} ${Silent}
          MessageBox MB_OK "Error: Miniconda installation failed. Installation will be aborted."
        ${EndIf}
        Goto exit_installer
      ${EndIf}

    create_env:
      DetailPrint "---------------------"
      DetailPrint "- Conda Environment -"
      DetailPrint "---------------------"

      DetailPrint "- Initializing conda..."
      ; Use the appropriate conda executable
      ${If} $R1 == ""
        StrCpy $R1 "conda"
      ${EndIf}
      ; Initialize conda (needed for systems where conda was previously installed but not initialized)
      nsExec::ExecToStack '"$R1" init'

      DetailPrint "- Creating a Python 3.10 environment named '$LEMONADE_CONDA_ENV' in the installation directory: $INSTDIR..."
      ExecWait '"$R1" create -p "$INSTDIR\$LEMONADE_CONDA_ENV" python=3.10 -y' $R0

      ; Check if the environment creation was successful (exit code should be 0)
      StrCmp $R0 0 install_lemonade env_creation_failed

    env_creation_failed:
      DetailPrint "- ERROR: Environment creation failed"
      ; Display an error message and exit
      ${IfNot} ${Silent}
        MessageBox MB_OK "ERROR: Failed to create the Python environment. Installation will be aborted."
      ${EndIf}
      Quit

    install_lemonade:
      DetailPrint "-------------------------"
      DetailPrint "- Lemonade Installation -"
      DetailPrint "-------------------------"


      DetailPrint "- Installing $LEMONADE_SERVER_STRING..."
      ${If} $HYBRID_SELECTED == "true"
        nsExec::ExecToLog '"$INSTDIR\$LEMONADE_CONDA_ENV\python.exe" -m pip install -e "$INSTDIR"[llm-oga-hybrid] --no-warn-script-location'
      ${Else}
        nsExec::ExecToLog '"$INSTDIR\$LEMONADE_CONDA_ENV\python.exe" -m pip install -e "$INSTDIR"[llm] --no-warn-script-location'
      ${EndIf}
      Pop $R0  ; Return value
      DetailPrint "- $LEMONADE_SERVER_STRING install return code: $R0"

      ; Check if installation was successful (exit code should be 0)
      StrCmp $R0 0 install_success install_failed

    install_success:
      DetailPrint "- $LEMONADE_SERVER_STRING installation successful"

      DetailPrint "*** INSTALLATION COMPLETED ***"
      # Create a shortcut inside $INSTDIR
      CreateShortcut "$INSTDIR\lemonade-server.lnk" "$SYSDIR\cmd.exe" "/C conda run --no-capture-output -p $INSTDIR\$LEMONADE_CONDA_ENV lemonade serve" "$INSTDIR\img\favicon.ico"

      Goto end

    install_failed:
      DetailPrint "- $LEMONADE_SERVER_STRING installation failed"
      ${IfNot} ${Silent}
        MessageBox MB_OK "ERROR: $LEMONADE_SERVER_STRING package failed to install using pip. Installation will be aborted."
      ${EndIf}
      Quit

    end:
SectionEnd

Section "Install Ryzen AI Hybrid Execution" HybridSec
  DetailPrint "------------------------"
  DetailPrint "- Ryzen AI Section     -"
  DetailPrint "------------------------"

  ; Once we're done downloading and installing the archive the size comes out to about 1GB
  AddSize 1048576

  nsExec::ExecToLog 'conda run --no-capture-output -p $INSTDIR\$LEMONADE_CONDA_ENV lemonade-install --ryzenai hybrid -y'

  Pop $R0  ; Return value
  DetailPrint "Hybrid execution mode install return code: $R0"

  ; Check if installation was successful (exit code should be 0)
  StrCmp $R0 0 end install_failed

  install_failed:
      DetailPrint "- Hybrid installation failed"
      ${IfNot} ${Silent}
        MessageBox MB_OK "ERROR: Hybrid mode failed to install using pip. Installation will be aborted."
      ${EndIf}
      Quit

  end:
SectionEnd

Section "-Add Desktop Shortcut" ShortcutSec  
  ; Create a desktop shortcut that passes the conda environment name as a parameter
  CreateShortcut "$DESKTOP\lemonade-server.lnk" "$INSTDIR\run_server.bat" "$LEMONADE_CONDA_ENV" "$INSTDIR\img\favicon.ico"

SectionEnd

Function RunServer
  ExecShell "open" "$INSTDIR\LEMONADE-SERVER.lnk"
FunctionEnd

; Define constants for better readability
!define ICON_FILE "..\img\favicon.ico"

; Finish Page settings
!define MUI_TEXT_FINISH_INFO_TITLE "$LEMONADE_SERVER_STRING installed successfully!"
!define MUI_TEXT_FINISH_INFO_TEXT "A shortcut has been added to your Desktop. What would you like to do next?"

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION RunServer
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_RUN_TEXT "Run Lemonade Server"

Function .onSelChange
    StrCpy $HYBRID_SELECTED "false"
    SectionGetFlags ${HybridSec} $0
    IntOp $0 $0 & ${SF_SELECTED}
    StrCmp $0 ${SF_SELECTED} 0 +2
    StrCpy $HYBRID_SELECTED "true"
    ;MessageBox MB_OK "Component 2 is selected"
FunctionEnd

Function SkipLicense
  ${IfNot} ${SectionIsSelected} ${HybridSec}
    abort  ;skip AMD license if hybrid was not enabled
  ${EndIf}
FunctionEnd


; MUI Settings
!insertmacro MUI_PAGE_WELCOME
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
LangString MUI_TEXT_LICENSE_SUBTITLE ${LANG_ENGLISH} "Please review the license terms before installing AMD Ryzen AI Hybrid Execution Mode."
LangString DESC_SEC01 ${LANG_ENGLISH} "The minimum set of dependencies for a lemonade server that runs LLMs on CPU."
LangString DESC_HybridSec ${LANG_ENGLISH} "Add support for running LLMs on Ryzen AI hybrid execution mode, which uses both the NPU and iGPU for improved performance on Ryzen AI 300-series processors."

; Insert the description macros
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC01} $(DESC_SEC01)
  !insertmacro MUI_DESCRIPTION_TEXT ${HybridSec} $(DESC_HybridSec)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  StrCpy $LEMONADE_SERVER_STRING "Lemonade Server"
  StrCpy $LEMONADE_CONDA_ENV "lemon_env"
  StrCpy $HYBRID_SELECTED "true"

  ; Set the install directory, allowing /D override from CLI install
  ${If} $InstDir != ""
    ; /D was used
  ${Else}
    ; Use the default
    StrCpy $InstDir "$LOCALAPPDATA\lemonade_server"
  ${EndIf}

  ; Disable hybrid mode by default in silent mode
  ; Use /Extras="hybrid" option to enable it
  ${If} ${Silent}
    
    ${GetParameters} $CMDLINE
    ${GetOptions} $CMDLINE "/Extras=" $HYBRID_CLI_OPTION

    ${IfNot} $HYBRID_CLI_OPTION == "hybrid"
      SectionSetFlags ${HybridSec} 0
      StrCpy $HYBRID_SELECTED "false"
    ${EndIf}
  ${EndIf}


FunctionEnd