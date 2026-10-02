@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "!VSWHERE!" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "VERDEF="
if defined VERSION set "VERDEF=/DCLIPPER_VERSION=%VERSION%"
rc /nologo clipper.rc || exit /b 1
cl /nologo /O1 /MT /EHsc /std:c++17 /W3 /utf-8 /DUNICODE /D_CRT_SECURE_NO_WARNINGS %VERDEF% clipper.cpp clipper.res /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /OUT:clipper.exe
exit /b %errorlevel%
