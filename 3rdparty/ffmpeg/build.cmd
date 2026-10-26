@echo off
rem Builds the static FFmpeg that WinUAE links, for one platform.
rem   build.cmd [x86|x64]      (default x86, i.e. Release|Win32)
rem Needs: Visual Studio with C++ (vcvars, bundled vcpkg), MSYS2 at C:\msys64
rem (make, pkgconf, diffutils), NASM on PATH. Output: 3rdparty\ffmpeg\<arch>\{include,lib}
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x86
set HERE=%~dp0
set HERE=%HERE:~0,-1%

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" ( echo no Visual Studio found & exit /b 1 )
if "%ARCH%"=="x86" ( call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul ) else ( call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul )
if errorlevel 1 exit /b 1

if exist "%ProgramFiles%\NASM\nasm.exe" set PATH=%ProgramFiles%\NASM;%PATH%

rem 1. external decoders, static with the static CRT (/MT) like winuae.exe
pushd "%HERE%"
"%VSDIR%\VC\vcpkg\vcpkg.exe" install --triplet %ARCH%-windows-static --x-install-root="%HERE%\deps" || ( popd & exit /b 1 )
popd

rem 2. FFmpeg itself, from the fork in src\ (submodule)
set MSYS2_PATH_TYPE=inherit
set MSYSTEM=MSYS
set CHERE_INVOKING=1
C:\msys64\usr\bin\bash.exe -l "%HERE%\build.sh" %ARCH%
exit /b %errorlevel%
