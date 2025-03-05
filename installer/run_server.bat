@echo off
set CONDA_ENV=%1
if "%CONDA_ENV%"=="" set CONDA_ENV=lemon_env
echo Starting Lemonade Server...
call conda run --no-capture-output -p "%~dp0%CONDA_ENV%" lemonade serve
if %ERRORLEVEL% neq 0 (
  echo.
  echo An error occurred while running Lemonade Server.
  echo Please check the error message above.
  echo.
  pause
)
