@echo off
rem SoftG build script (ASCII only for cmd compatibility)
rem Usage: scripts\build.cmd [preset] [target]
rem   preset: x64-debug (default) | x64-release
rem   target: optional, e.g. softg_tests / LogicPlanner / PageViewer
setlocal
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set VSROOT=%%i
if not defined VSROOT (
    echo [build.cmd] Visual Studio not found
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cmake >nul 2>nul || set PATH=%PATH%;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
set PRESET=%1
if "%PRESET%"=="" set PRESET=x64-debug
cmake --preset %PRESET% || exit /b 1
if "%2"=="" (cmake --build --preset %PRESET%) else (cmake --build --preset %PRESET% --target %2)
exit /b %ERRORLEVEL%
