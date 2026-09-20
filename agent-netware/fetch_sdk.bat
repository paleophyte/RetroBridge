@echo off
setlocal
REM Compatibility entry point: prepare a local SDK; no downloads.
if "%~1"=="" (
  echo Usage: fetch_sdk.bat "C:\path\to\novell-clib-devel-2007.10.02-1netware_windows"
  echo Obtain an SDK you are entitled to use. See ..\THIRD_PARTY.md.
  exit /b 1
)
set "NLM_PYTHON=python"
if exist "%~dp0..\mcp-server\.venv\Scripts\python.exe" set "NLM_PYTHON=%~dp0..\mcp-server\.venv\Scripts\python.exe"
if not defined NLM_SDK_DIR set "NLM_SDK_DIR=%~dp0..\.deps\netware-sdk"
"%NLM_PYTHON%" "%~dp0..\tools\prepare_dependencies.py" --component netware-sdk --source-root "%~1" --destination "%NLM_SDK_DIR%"
exit /b %errorlevel%
