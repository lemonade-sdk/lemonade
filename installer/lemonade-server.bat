@echo off
setlocal enabledelayedexpansion

REM --keep-alive is only used by the bash script to make sure that, if the server fails to open, we don't close the terminal right away.
REM Check for --keep-alive argument and remove it from arguments passed to CLI
set KEEP_ALIVE=0
set ARGS=
for %%a in (%*) do (
    if /I "%%a"=="--keep-alive" (
        set KEEP_ALIVE=1
    ) else (
        set ARGS=!ARGS! %%a
    )
)

REM Change to parent directory where conda env and bin folders are located
pushd "%~dp0.."

REM Run the Python CLI script through conda, passing filtered arguments
call "%CD%\python\Scripts\lemonade-server-dev" !ARGS!
popd

REM Error handling: Show message and pause if --keep-alive was specified
if %ERRORLEVEL% neq 0 (
    if %KEEP_ALIVE%==1 (
        echo.
        echo An error occurred while running Lemonade Server.
        echo Please check the error message above.
        echo.
        pause
    )
)

REM This file was originally licensed under Apache 2.0. It has been modified.
REM Modifications Copyright (c) 2025 AMD 