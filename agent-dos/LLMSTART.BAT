@ECHO OFF
REM Run with CALL after the packet driver and network setup.
REM Optional first argument: agent directory (8.3 path, no spaces).
SET LLMHOME=C:\LLMAGENT
IF NOT "%1"=="" SET LLMHOME=%1
ECHO.
REM Only result 2 (A) starts the agent; failures and C keep the console.
CHOICE /C:CA /T:A,5 Console or agent
IF ERRORLEVEL 3 GOTO CONSOLE
IF NOT ERRORLEVEL 2 GOTO CONSOLE
IF NOT EXIST %LLMHOME%\LLMAGENT.EXE GOTO MISSING
IF NOT EXIST %LLMHOME%\LLMAGENT.INI GOTO MISSING
IF NOT EXIST %LLMHOME%\WATTCP.CFG GOTO MISSING
SET WATTCP.CFG=%LLMHOME%
ECHO Starting agent. Local Ctrl+C returns to the console.
%LLMHOME%\LLMAGENT.EXE
GOTO CONSOLE
:MISSING
ECHO Agent executable or configuration missing in %LLMHOME%.
:CONSOLE
ECHO Console mode. Remote agent access is stopped.
ECHO Run CALL %0 %LLMHOME% to start again.
SET LLMHOME=
