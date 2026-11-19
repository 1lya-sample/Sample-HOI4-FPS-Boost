@echo off
if "%~1"=="" (
    start "" "%~dp0dist\launcher.exe"
    exit /b 0
)
"%~dp0dist\launcher.exe" %*
