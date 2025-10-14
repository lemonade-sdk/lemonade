@echo off
setlocal enabledelayedexpansion

rem This is a simple wrapper to launch the Lemonade Server in GUI mode.
rem It calls the main batch script with the "serve" command, ensuring the tray icon appears.

REM Change to parent directory where conda env and bin folders are located
pushd "%~dp0.."

REM Run the Python CLI script, passing filtered arguments
call "%CD%\python\Scripts\lemonade-server-dev" serve
set SERVER_ERRORLEVEL=%ERRORLEVEL%
popd

