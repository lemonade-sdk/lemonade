@echo off
setlocal enabledelayedexpansion

REM Show a notification and run the server in tray mode.
REM Note: command line arguments are parsed in order from left to right
set TRAY=0
set ARGS=
for %%a in (%*) do (
    set ARGS=!ARGS! %%a
    if /I "%%a"=="serve" (
        set TRAY=1
    )
    if /I "%%a"=="--no-tray" (
        set TRAY=0
    )
)

REM Change to parent directory where conda env and bin folders are located
pushd "%~dp0.."

if %TRAY%==1 (
    REM Non-blocking call to show notification
    start /min wscript "%~dp0lemonade_notification.vbs" "Lemonade Server" "Initializing Lemonade Server...\nThis window will close automatically when the server is ready."
)

REM Print exactly the command that will be run
echo "Running: lemonade-server-dev %ARGS%"

REM Run the Python CLI script, passing filtered arguments
call "%CD%\python\Scripts\lemonade-server-dev" !ARGS!
set SERVER_ERRORLEVEL=%ERRORLEVEL%
popd

if %TRAY%==1 (
    REM Close the loading notification
    wmic process where "name='wscript.exe' and commandline like '%%lemonade_notification.vbs%%'" delete >nul 2>&1
)

REM Provide a notification if the server is already running
if %SERVER_ERRORLEVEL% equ 2 (
    if %TRAY%==1 (
        REM Blocking call to show notification
        wscript "%~dp0lemonade_notification.vbs" "Lemonade Server" "Lemonade Server is already running!\nCheck your system tray for details or run `lemonade-server stop` to stop the existing server and try again."
        exit /b 2
    )
)

REM Exit without additional notifications if error code is 0 (no errors), 15 (lemonade-server stop), or less than 0 (forced exit)
if %SERVER_ERRORLEVEL% equ 15 (
    exit /b 15
) else if %SERVER_ERRORLEVEL% leq 0 (
    exit /b 0
)

REM Error handling if any other error code
if %TRAY%==0 (
    echo.
    echo An error occurred while running Lemonade Server.
    echo Please check the error message above.
    echo.
    pause
)
if %TRAY%==1 (
    REM Blocking call to show notification
    wscript "%~dp0lemonade_notification.vbs" "Lemonade Server" "An error occurred while running Lemonade Server.\nPlease run the server manually. Error code: %SERVER_ERRORLEVEL%"
)

REM This file was originally licensed under Apache 2.0. It has been modified.
REM Modifications Copyright (c) 2025 AMD 