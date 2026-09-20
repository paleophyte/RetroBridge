@echo off
REM Called inside build.bat's SETLOCAL; keep these variables in its scope.
set "NLM_PYTHON=python"
if exist "%~dp0..\mcp-server\.venv\Scripts\python.exe" set "NLM_PYTHON=%~dp0..\mcp-server\.venv\Scripts\python.exe"
if not defined NLM_SDK_DIR set "NLM_SDK_DIR=%~dp0..\.deps\netware-sdk"
"%NLM_PYTHON%" "%~dp0..\tools\prepare_dependencies.py" --component netware-sdk --check --destination "%NLM_SDK_DIR%"
exit /b %errorlevel%
