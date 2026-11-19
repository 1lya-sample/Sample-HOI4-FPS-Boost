@echo off
if "%~1"=="" (
    start "" "%~dp0bin\launcher.exe"
    exit /b 0
)
"%~dp0bin\launcher.exe" %*
