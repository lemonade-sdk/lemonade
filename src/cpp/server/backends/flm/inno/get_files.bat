@echo off
echo Copying files to inno folder...

REM Copy flm.exe from build folder
echo Copying flm.exe...
copy "..\build\flm.exe" "flm.exe"

REM Copy all DLL files from lib folder
echo Copying DLL files...
copy "..\lib\*.dll" "."

REM Copy model_list.json from root
echo Copying model_list.json...
copy "..\model_list.json" "model_list.json"

echo Done!
