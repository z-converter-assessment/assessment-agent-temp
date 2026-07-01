@echo off
REM ============================================================
REM  Assessment Agent (Windows) - Server 2003 installer
REM
REM  Server 2003 has no PowerShell by default, so this .bat is the
REM  install entry point for the LEGACY (NT 5.2) build. On modern
REM  Windows you can equally just run `assessment-agent.exe install`.
REM
REM  All real work (service create, env seeding from the embedded
REM  resource, auto-restart policy) is done by the exe's `install`
REM  subcommand via CreateServiceW / ChangeServiceConfig2W, all of
REM  which exist on NT 5.2. This script only checks admin rights and
REM  forwards the command, with a raw sc.exe fallback for diagnosis.
REM
REM  Usage (from an elevated Command Prompt):
REM      install.bat                 install + start
REM      install.bat --image-prep    register but do NOT start
REM                                  (seal into a golden image, then
REM                                   run `assessment-agent.exe prep-image`)
REM ============================================================
setlocal

set "SCRIPT_DIR=%~dp0"

REM The Server 2003 binary (x86 — the only 2003 target; the x64 Edition is
REM not built) ships next to this .bat.
set "EXE=%SCRIPT_DIR%assessment-agent-win2003-x86.exe"
if not exist "%EXE%" (
    echo [install] agent exe not found next to install.bat
    exit /b 1
)

REM --- Admin check: `net session` only succeeds for an elevated token. ---
net session >nul 2>&1
if errorlevel 1 (
    echo [install] must run from an elevated Command Prompt ^(Run as administrator^).
    exit /b 1
)

echo [install] using %EXE%
"%EXE%" install %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo [install] exe install subcommand failed ^(rc=%RC%^).
    echo [install] manual fallback - register the service directly with sc.exe:
    echo     sc create assessment-agent binPath= "%EXE%" start= auto DisplayName= "Assessment Agent"
    echo     sc description assessment-agent "Assessment Agent - collector + task.install worker"
    echo     sc failure assessment-agent reset= 86400 actions= restart/5000/restart/10000/restart/30000
    echo     sc start assessment-agent
    echo.
    echo [install] uninstall later with:  "%EXE%" uninstall
    endlocal
    exit /b %RC%
)

echo [install] done. service 'assessment-agent' registered and started.
echo [install] uninstall with:  "%EXE%" uninstall
endlocal
