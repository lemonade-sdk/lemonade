@echo off
setlocal enabledelayedexpansion

REM Get current time in milliseconds since midnight
for /f "tokens=1-4 delims=:.," %%a in ("!time!") do (
    set /a "CURRENT_TIME=((((%%a*60)+1%%b %% 100)*60+1%%c %% 100)*1000)+1%%d %% 1000"
)

REM Use temp directory for the lock file
set "LOCK_FILE=%TEMP%\lemonade_server.lock"

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

REM Only check lock file if running in tray mode
if %TRAY%==1 (
    REM Check if another instance is starting (within last 10000 milliseconds)
    if exist "!LOCK_FILE!" (
        set /p STORED_TIME=<"!LOCK_FILE!"
        set /a TIME_DIFF=!CURRENT_TIME!-!STORED_TIME!
        
        REM Only block if difference is positive and less than 10000 milliseconds (10 seconds)
        if !TIME_DIFF! gtr 0 if !TIME_DIFF! lss 10000 (
            echo Another instance of Lemonade Server is currently starting.
            exit /b 3
        )
    )

    REM Set the starting timestamp in lock file
    echo !CURRENT_TIME!>"!LOCK_FILE!"
)

REM Change to parent directory where conda env and bin folders are located
pushd "%~dp0.."

REM Run the Python CLI script, passing filtered arguments
call "%CD%\python\Scripts\lemonade-server-dev" !ARGS!
set SERVER_ERRORLEVEL=%ERRORLEVEL%
popd

REM Clean up lock file before any exit
del "!LOCK_FILE!" 2>nul

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