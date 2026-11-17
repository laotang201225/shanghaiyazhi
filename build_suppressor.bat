@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist "_build" mkdir "_build"
set "OUT=gaoDamageSuppressor"
cl /nologo /std:c++17 /utf-8 /W4 /WX /O2 /EHsc /MT /LD /Fo"_build\%OUT%.obj" DamageSuppressor.cpp user32.lib /Fe"_build\%OUT%.dll"
exit /b %errorlevel%
