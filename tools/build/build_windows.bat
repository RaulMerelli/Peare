@echo off
setlocal EnableExtensions

title Build Peare - Qt 5.15.2 / MSVC x64

for %%I in ("%~dp0..\..") do set "ROOT_DIR=%%~fI"
cd /d "%ROOT_DIR%"

git config --local core.hooksPath .githooks
if errorlevel 1 (
    echo ERROR: Could not enable the repository commit and push gates.
    goto :fail
)

set "QT_ROOT=C:\Qt\5.15.2\msvc2019_64"
set "BUILD_DIR=build-windows"
set "EXE_PATH=%BUILD_DIR%\Release\Peare.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

echo.
echo === Peare: build Release ===
echo Project: %CD%
echo.

if not exist "CMakeLists.txt" (
    echo ERROR: CMakeLists.txt not found.
    goto :fail
)

if not exist "%QT_ROOT%\bin\qmake.exe" (
    echo ERROR: Qt 5.15.2 not found in:
    echo %QT_ROOT%
    goto :fail
)

if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found.
    echo Verify Visual Studio installation.
    goto :fail
)

where ninja >nul 2>nul
if errorlevel 1 (
    echo ERROR: Ninja was not found in PATH.
    echo Install Ninja and add it to PATH.
    goto :fail
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%I"
)

if not defined VS_PATH (
    echo ERROR: Visual Studio with C++ compiler not found.
    goto :fail
)

echo Initializing the MSVC x64 compiler...
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail

echo.
echo Configurating CMake...
cmake -S . -B "%BUILD_DIR%" ^
  -G "Ninja Multi-Config" ^
  -DCMAKE_PREFIX_PATH="%QT_ROOT%" ^
  -DPEARE_QT_MAJOR=5 ^
  -DPEARE_CXX_STANDARD=11
if errorlevel 1 goto :fail

echo.
echo Compiling Release...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 goto :fail

if not exist "%EXE_PATH%" (
    echo ERROR: executable not generated:
    echo %EXE_PATH%
    goto :fail
)

echo.
echo Copy Qt DLLs...
"%QT_ROOT%\bin\windeployqt.exe" --release --no-compiler-runtime --no-angle --no-opengl-sw --no-system-d3d-compiler --no-translations "%EXE_PATH%"
if errorlevel 1 goto :fail

if not exist "%QT_ROOT%\plugins\platforms\qoffscreen.dll" (
    echo ERROR: Qt offscreen platform plugin not found.
    goto :fail
)
if not exist "%BUILD_DIR%\test-plugins\platforms" mkdir "%BUILD_DIR%\test-plugins\platforms"
copy /y "%QT_ROOT%\plugins\platforms\qoffscreen.dll" "%BUILD_DIR%\test-plugins\platforms\qoffscreen.dll" >nul
if errorlevel 1 goto :fail

for %%F in (libEGL.dll libGLESv2.dll opengl32sw.dll d3dcompiler_47.dll PeareExtCasefoldTests.exe) do (
    if exist "%BUILD_DIR%\Release\%%F" del /q "%BUILD_DIR%\Release\%%F"
)
if exist "%BUILD_DIR%\Release\platforms\qoffscreen.dll" del /q "%BUILD_DIR%\Release\platforms\qoffscreen.dll"
for %%F in ("%BUILD_DIR%\Release\translations\*.qm") do if exist "%%~fF" del /q "%%~fF"
if exist "%BUILD_DIR%\Release\translations" rmdir "%BUILD_DIR%\Release\translations" 2>nul

echo.
echo === BUILD COMPLETED ===
echo Executable:
echo %CD%\%EXE_PATH%
echo.
if not "%PEARE_NO_PAUSE%"=="1" pause
exit /b 0

:fail
echo.
echo === BUILD FAILED ===
echo Check the error message above.
echo.
if not "%PEARE_NO_PAUSE%"=="1" pause
exit /b 1
