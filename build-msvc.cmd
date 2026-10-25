@echo off
setlocal enabledelayedexpansion
set ROOT=%~dp0
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" ( echo ERROR: vswhere not found & exit /b 1 )
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.ATL -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" ( echo ERROR: no VS install with the C++ ATL component & exit /b 1 )
set MSBUILD=%VSDIR%\MSBuild\Current\Bin\MSBuild.exe
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set PLATFORM=%2
if "%PLATFORM%"=="" set PLATFORM=Win32
echo === WinUAE %CONFIG%^|%PLATFORM% ===
echo === VS: %VSDIR%
"%MSBUILD%" "%ROOT%od-win32\winuae_msvc15\winuae_msvc.sln" /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /m /v:minimal /nologo
set RC=%ERRORLEVEL%
echo === exit code: %RC% ===
exit /b %RC%
