@echo off
setlocal EnableExtensions

title Build Peare - Qt 5.15.2 / MSVC x64

cd /d "%~dp0"

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

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%I"
)

if not defined VS_PATH (
    echo ERROR: Visual Studio with C++ compiler not found.
    goto :fail
)

if not exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: vcvars64.bat not found in:
    echo %VS_PATH%
    goto :fail
)

echo Inizializzazione compilatore MSVC x64...
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail

echo.
echo Configurating CMake...
cmake -S . -B "%BUILD_DIR%" ^
  -DCMAKE_PREFIX_PATH="%QT_ROOT%" ^
  -DPEARE_QT_MAJOR=5 ^
  -DPEARE_CXX_STANDARD=11
if errorlevel 1 goto :fail

echo.
echo Compiling Release...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 goto :fail

if not exist "%EXE_PATH%" (
    echo ERRORE: executable not generated:
    echo %EXE_PATH%
    goto :fail
)

echo.
echo Copy Qt DLLs...
"%QT_ROOT%\bin\windeployqt.exe" --release --no-compiler-runtime "%EXE_PATH%"
if errorlevel 1 goto :fail

echo.
echo === BUILD COMPLETED ===
echo Executable:
echo %CD%\%EXE_PATH%
echo.
pause
exit /b 0

:fail
echo.
echo === BUILD FAILED ===
echo Check the error message above.
echo.
pause
exit /b 1
