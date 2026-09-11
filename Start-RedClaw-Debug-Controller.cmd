@echo off
setlocal
cd /d "%~dp0"

where pwsh.exe >nul 2>nul
if errorlevel 1 (
    echo PowerShell 7 ^(pwsh.exe^) was not found in PATH.
    echo Install PowerShell 7 or add it to PATH, then retry.
    pause
    exit /b 1
)

pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\service\start-debug-controller.ps1"
if errorlevel 1 (
    echo.
    echo RedClaw Debug Controller failed to start. Review the error above.
    pause
    exit /b 1
)

endlocal
