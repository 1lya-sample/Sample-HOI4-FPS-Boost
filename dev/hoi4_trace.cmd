@echo off
rem hoi4_trace.cmd <name> -> D:\hoi4_<name>.etl (as administrator)

whoami /groups | find "S-1-16-12288" >nul
if errorlevel 1 (
    echo Needs administrator rights: right click this file -^> Run as administrator.
    pause
    exit /b 1
)

set NAME=%~1
if "%NAME%"=="" set /p NAME=Trace name (for example pause or speed5):
if "%NAME%"=="" set NAME=trace

wpr -cancel >nul 2>&1
echo Switch to the game now. Recording starts in 5 seconds and lasts 15 seconds.
timeout /t 5 /nobreak >nul
wpr -start CPU -filemode
if errorlevel 1 ( pause & exit /b 1 )
echo Recording...
timeout /t 15 /nobreak >nul
echo Saving, this takes a minute. Do not close this window.
wpr -stop "D:\hoi4_%NAME%.etl"
echo Done: D:\hoi4_%NAME%.etl
pause
