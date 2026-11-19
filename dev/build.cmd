@echo off
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
set SRC=%~dp0..\src
set MI=%~dp0third_party\mimalloc
set BIN=%~dp0..\bin
set OUT=%~dp0build
set OBJ=%~dp0build\obj

if not exist "%MI%\src\static.c" (
    git -c advice.detachedHead=false clone --depth 1 --branch v2.1.7 https://github.com/microsoft/mimalloc.git "%MI%" || exit /b 1
)
call "%VCVARS%" >nul 2>nul || (echo Visual Studio C++ tools not found: %VCVARS% & exit /b 1)
if not exist "%OBJ%\dll" mkdir "%OBJ%\dll"
set CL_FLAGS=/nologo /utf-8 /O2 /MT /EHsc /DNDEBUG /W3 /DUNICODE /D_UNICODE

cl %CL_FLAGS% /c /DMI_DEBUG=0 /I"%MI%\include" /Fo"%OBJ%\mimalloc.obj" "%MI%\src\static.c" || exit /b 1
cl %CL_FLAGS% /LD /I"%MI%\include" /Fo"%OBJ%\dll\\" "%SRC%\hoi4boost\*.cpp" "%OBJ%\mimalloc.obj" /Fe"%OBJ%\hoi4boost.dll" /link advapi32.lib user32.lib || exit /b 1
copy /y "%OBJ%\hoi4boost.dll" "%BIN%\hoi4boost.dll" >nul 2>nul || echo WARNING: bin\hoi4boost.dll is in use by the running game; close the game and run build.cmd again to update it

cl %CL_FLAGS% /Fo"%OBJ%\launcher.obj" "%SRC%\launcher\launcher.cpp" /Fe"%BIN%\launcher.exe" /link /SUBSYSTEM:WINDOWS shell32.lib user32.lib || exit /b 1
cl %CL_FLAGS% /Fo"%OBJ%\patchtest.obj" "%SRC%\tools\patchtest.cpp" /Fe"%OUT%\patchtest.exe" || exit /b 1
rem the boost only activates in a process named hoi4.exe
cl %CL_FLAGS% /Fo"%OBJ%\stress.obj" "%SRC%\tools\stress.cpp" /Fe"%OUT%\hoi4.exe" || exit /b 1
echo Build OK
