@echo off
setlocal
cd /d "%~dp0"

rem Repository settings are environment variables set by config.bat.
call "%~dp0config.bat"
call "%VCVARS64%" >nul 2>&1
if errorlevel 1 exit /b 1

if not exist out mkdir out
if errorlevel 1 exit /b 1

cl /nologo /O2 /W4 /GS- /Gs1048576 /Oi- /Zi /Fdout\vc140.pdb causext.c /Foout\causext.obj /Fe:out\causext.exe /link /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup /NODEFAULTLIB /DEBUG:FULL /OPT:NOREF /INCREMENTAL:NO /PDB:out\causext.pdb kernel32.lib advapi32.lib shell32.lib
if errorlevel 1 exit /b 1

if /I "%~1"=="run" (
    out\causext.exe --self-test
    exit /b %errorlevel%
)
exit /b 0
