@echo off
setlocal
cd /d "%~dp0"

set "ZIP=%~dp0vendor\novell-clib-devel.zip"
set "OUT=%~dp0vendor\clib-sdk"
set "SDK=%OUT%\novell-clib-devel-2007.10.02-1netware_windows"
set "URL=https://archive.org/download/novell-developer-kit-oct-2007/novell%%20developer%%20kit%%20dvd%%20october%%202007%%20web%%20release.iso/downloads%%2Fclib%%2Fnovell-clib-devel-2007.10.02-1netware_windows.zip"

if not exist vendor mkdir vendor
if not exist vendor\imports mkdir vendor\imports

if not exist "%ZIP%" (
  echo Downloading CLIB NDK zip from Internet Archive...
  curl.exe -L --fail --max-time 600 -o "%ZIP%" "%URL%"
  if errorlevel 1 (
    echo ERROR: download failed
    exit /b 1
  )
)

echo Extracting...
powershell -NoProfile -Command "Expand-Archive -Force -Path '%ZIP%' -DestinationPath '%OUT%'"
if errorlevel 1 exit /b 1

if not exist "%SDK%\imports\clib.imp" (
  echo ERROR: clib.imp not found after extract
  exit /b 1
)
copy /y "%SDK%\imports\clib.imp" vendor\imports\clib.imp >nul
for %%F in (prelude.obj nwpre.obj clibpre.obj threads.imp nlmlib.imp) do (
  if exist "%SDK%\imports\%%F" copy /y "%SDK%\imports\%%F" vendor\imports\%%F >nul
)
echo Updated vendor\imports\
dir vendor\imports\clib.imp vendor\imports\prelude.obj
