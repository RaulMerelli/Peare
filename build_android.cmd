@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Build Peare for Android from Windows with a Qt 6 Android kit.
rem
rem Environment variables:
rem   QT_HOST_PATH     Matching desktop Qt, for example C:\Qt\6.7.3\msvc2019_64
rem   ANDROID_SDK_ROOT Android SDK root
rem   ANDROID_NDK_ROOT Android NDK root
rem   JAVA_HOME        JDK 17 root; Gradle 8.3 cannot run on newer JDKs
rem   PEARE_ANDROID_PLATFORM Android compile platform; defaults to android-34
rem   PEARE_ANDROID_RELEASE Set to 1 only when release-signing variables are configured
rem
rem Optional positional arguments:
rem   1 Qt Android kit
rem   2 build type
rem   3 build directory
rem
rem Example:
rem   set QT_HOST_PATH=C:\Qt\6.7.3\msvc2019_64
rem   set ANDROID_SDK_ROOT=%LOCALAPPDATA%\Android\Sdk
rem   set ANDROID_NDK_ROOT=%ANDROID_SDK_ROOT%\ndk\26.1.10909125
rem   build_android.cmd

set "QT_ANDROID_KIT=%~1"
set "BUILD_TYPE=%~2"
set "BUILD_DIR=%~3"

if not defined QT_ANDROID_KIT set "QT_ANDROID_KIT=C:\Qt\6.7.3\android_arm64_v8a"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if not defined BUILD_DIR set "BUILD_DIR=build-android"
if not defined PEARE_ANDROID_PLATFORM set "PEARE_ANDROID_PLATFORM=android-34"
if not defined ANDROID_ABI set "ANDROID_ABI=arm64-v8a"

if not defined QT_ANDROID_KIT (
    echo ERROR: Qt Android kit not specified.
    echo Pass it as the first argument or edit the default QT_ANDROID_KIT path.
    goto fail
)

for %%I in ("%QT_ANDROID_KIT%") do set "QT_ANDROID_KIT=%%~fI"
set "QT_CMAKE=%QT_ANDROID_KIT%\bin\qt-cmake.bat"
if not exist "%QT_CMAKE%" (
    echo ERROR: qt-cmake.bat not found:
    echo   %QT_CMAKE%
    echo Use a Qt 6 Android kit directory, not a desktop Qt directory.
    goto fail
)

rem Derive the matching Qt version directory, then try common Windows host kits.
if not defined QT_HOST_PATH (
    for %%I in ("%QT_ANDROID_KIT%\..") do set "QT_VERSION_DIR=%%~fI"
    for %%H in (msvc2022_64 msvc2019_64 mingw_64) do (
        if not defined QT_HOST_PATH if exist "!QT_VERSION_DIR!\%%H\bin\moc.exe" set "QT_HOST_PATH=!QT_VERSION_DIR!\%%H"
    )
)
if not defined QT_HOST_PATH (
    echo ERROR: QT_HOST_PATH is not set and no matching desktop Qt was found.
    echo Install the desktop kit for the same Qt version, then set for example:
    echo   set QT_HOST_PATH=C:\Qt\6.7.3\msvc2019_64
    goto fail
)
for %%I in ("%QT_HOST_PATH%") do set "QT_HOST_PATH=%%~fI"
if not exist "%QT_HOST_PATH%\bin\moc.exe" (
    echo ERROR: QT_HOST_PATH is not a usable desktop Qt installation:
    echo   %QT_HOST_PATH%
    goto fail
)

if not defined ANDROID_SDK_ROOT (
    if defined ANDROID_HOME set "ANDROID_SDK_ROOT=!ANDROID_HOME!"
)
if not defined ANDROID_SDK_ROOT (
    if defined LOCALAPPDATA (
        set "DEFAULT_ANDROID_SDK=!LOCALAPPDATA!\Android\Sdk"
        if exist "!DEFAULT_ANDROID_SDK!\." set "ANDROID_SDK_ROOT=!DEFAULT_ANDROID_SDK!"
    )
)
if not defined ANDROID_SDK_ROOT (
    set "DEFAULT_ANDROID_SDK=!USERPROFILE!\AppData\Local\Android\Sdk"
    if exist "!DEFAULT_ANDROID_SDK!\." set "ANDROID_SDK_ROOT=!DEFAULT_ANDROID_SDK!"
)
if not defined ANDROID_SDK_ROOT (
    echo ERROR: ANDROID_SDK_ROOT is not set and the default SDK was not found.
    echo Example:
    echo   set ANDROID_SDK_ROOT=%%LOCALAPPDATA%%\Android\Sdk
    goto fail
)
for %%I in ("%ANDROID_SDK_ROOT%") do set "ANDROID_SDK_ROOT=%%~fI"
if not exist "%ANDROID_SDK_ROOT%\platform-tools" (
    echo ERROR: Invalid Android SDK root:
    echo   %ANDROID_SDK_ROOT%
    goto fail
)

if not exist "%ANDROID_SDK_ROOT%\platforms\%PEARE_ANDROID_PLATFORM%\android.jar" (
    echo ERROR: Android SDK platform %PEARE_ANDROID_PLATFORM% is not installed.
    echo Qt 6.7 uses Android Gradle Plugin 7.4.1, which is tested through API 33, but current AndroidX dependencies require API 34.
    echo Install it with sdkmanager, for example:
    echo   "%%ANDROID_SDK_ROOT%%\cmdline-tools\latest\bin\sdkmanager.bat" "platforms;%PEARE_ANDROID_PLATFORM%"
    goto fail
)

rem Qt 6.7 official Android packages were built with NDK r26b.
rem Prefer that exact side-by-side NDK instead of blindly selecting the newest one.
if not defined ANDROID_NDK_ROOT (
    if exist "!ANDROID_SDK_ROOT!\ndk\26.1.10909125\build\cmake\android.toolchain.cmake" (
        set "ANDROID_NDK_ROOT=!ANDROID_SDK_ROOT!\ndk\26.1.10909125"
    )
)
rem Fall back to another installed NDK only when r26b is unavailable.
if not defined ANDROID_NDK_ROOT (
    set "NDK_PARENT=!ANDROID_SDK_ROOT!\ndk"
    if exist "!NDK_PARENT!\." (
        for /f "delims=" %%N in ('dir /b /ad /o-n "!NDK_PARENT!" 2^>nul') do (
            if not defined ANDROID_NDK_ROOT (
                if exist "!NDK_PARENT!\%%N\build\cmake\android.toolchain.cmake" (
                    set "ANDROID_NDK_ROOT=!NDK_PARENT!\%%N"
                )
            )
        )
    )
)
if not defined ANDROID_NDK_ROOT (
    if exist "!ANDROID_SDK_ROOT!\ndk-bundle\build\cmake\android.toolchain.cmake" (
        set "ANDROID_NDK_ROOT=!ANDROID_SDK_ROOT!\ndk-bundle"
    )
)
if not defined ANDROID_NDK_ROOT (
    echo ERROR: ANDROID_NDK_ROOT is not set and no installed NDK was found.
    echo Android SDK detected at:
    echo   !ANDROID_SDK_ROOT!
    echo Searched:
    echo   !ANDROID_SDK_ROOT!\ndk\^<version^>
    echo   !ANDROID_SDK_ROOT!\ndk-bundle
    echo.
    echo Installed SDK directories:
    dir /b /ad "!ANDROID_SDK_ROOT!" 2^>nul
    echo.
    echo Install Android SDK Command-line Tools and NDK ^(Side by side^)
    echo from Android Studio SDK Manager, or set ANDROID_NDK_ROOT explicitly.
    goto fail
)
for %%I in ("%ANDROID_NDK_ROOT%") do set "ANDROID_NDK_ROOT=%%~fI"
set "ANDROID_TOOLCHAIN=%ANDROID_NDK_ROOT%\build\cmake\android.toolchain.cmake"
if not exist "%ANDROID_TOOLCHAIN%" (
    echo ERROR: Android NDK toolchain file not found:
    echo   %ANDROID_TOOLCHAIN%
    goto fail
)

rem Qt 6.7 uses JDK 17 and its Gradle 8.3 wrapper cannot run on Java 25.
rem Reuse JAVA_HOME only when it points to JDK 17, otherwise search common locations.
set "JAVA_17_HOME="
if defined JAVA_HOME call :try_jdk17 "%JAVA_HOME%"
if not defined JAVA_17_HOME if exist "C:\Program Files\Android\Android Studio\jbr\bin\java.exe" call :try_jdk17 "C:\Program Files\Android\Android Studio\jbr"
if not defined JAVA_17_HOME for /d %%J in ("C:\Program Files\Eclipse Adoptium\jdk-17*") do if not defined JAVA_17_HOME call :try_jdk17 "%%~fJ"
if not defined JAVA_17_HOME for /d %%J in ("C:\Program Files\Microsoft\jdk-17*") do if not defined JAVA_17_HOME call :try_jdk17 "%%~fJ"
if not defined JAVA_17_HOME for /d %%J in ("C:\Program Files\Java\jdk-17*") do if not defined JAVA_17_HOME call :try_jdk17 "%%~fJ"
if not defined JAVA_17_HOME (
    echo ERROR: JDK 17 was not found.
    echo Gradle 8.3 from Qt 6.7 cannot run with Java 25 ^(class major version 69^).
    echo Install a JDK 17, for example with:
    echo   winget install EclipseAdoptium.Temurin.17.JDK
    echo Then rerun this script. JAVA_HOME is detected automatically.
    echo To set it manually:
    echo   set "JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-17.x.x"
    goto fail
)
set "JAVA_HOME=%JAVA_17_HOME%"
set "PATH=%JAVA_HOME%\bin;%PATH%"

where ninja.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: ninja.exe was not found in PATH.
    echo Add the Ninja shipped with Qt Creator or another Ninja installation.
    goto fail
)

pushd "%~dp0" || goto fail
set "PEARE_PUSHD=1"

rem A prior failed configure may cache a host compiler or a stale NDK path.
rem Start clean by default. Set PEARE_REUSE_ANDROID_BUILD=1 to reuse the tree.
if exist "%BUILD_DIR%" if not "%PEARE_REUSE_ANDROID_BUILD%"=="1" (
    echo Removing stale Android build directory: %BUILD_DIR%
    rmdir /s /q "%BUILD_DIR%"
)

set "ANDROID_HOME=%ANDROID_SDK_ROOT%"

echo.
echo Qt Android kit : %QT_ANDROID_KIT%
echo Qt host path   : %QT_HOST_PATH%
echo Android SDK    : %ANDROID_SDK_ROOT%
echo Android NDK    : %ANDROID_NDK_ROOT%
echo Java JDK       : %JAVA_HOME%
echo Android ABI    : %ANDROID_ABI%
echo Android API    : %PEARE_ANDROID_PLATFORM%
echo Build directory: %BUILD_DIR%
echo.

call "%QT_CMAKE%" -S "." -B "%BUILD_DIR%" -G "Ninja" ^
    "-DCMAKE_BUILD_TYPE=%BUILD_TYPE%" ^
    "-DQT_HOST_PATH=%QT_HOST_PATH%" ^
    "-DANDROID_SDK_ROOT=%ANDROID_SDK_ROOT%" ^
    "-DANDROID_NDK_ROOT=%ANDROID_NDK_ROOT%" ^
    "-DCMAKE_ANDROID_NDK=%ANDROID_NDK_ROOT%" ^
    "-DQT_CHAINLOAD_TOOLCHAIN_FILE=%ANDROID_TOOLCHAIN%" ^
    "-DANDROID_ABI=%ANDROID_ABI%" ^
    "-DPEARE_QT_MAJOR=6" ^
    "-DPEARE_CXX_STANDARD=AUTO"
if errorlevel 1 (
    echo.
    echo === ANDROID CONFIGURATION FAILED ===
    goto fail
)

cmake --build "%BUILD_DIR%" --target Peare --parallel
if errorlevel 1 (
    echo.
    echo === ANDROID COMPILATION FAILED ===
    goto fail
)

rem Qt 6.7 defaults androiddeployqt to the newest installed SDK platform.
rem Its Android Gradle Plugin 7.4.1 cannot reliably consume android-36, so run
rem androiddeployqt explicitly and pin the compile platform to android-34.
set "ANDROID_DEPLOY_QT=%QT_HOST_PATH%\bin\androiddeployqt.exe"
if not exist "%ANDROID_DEPLOY_QT%" set "ANDROID_DEPLOY_QT=%QT_HOST_PATH%\bin\androiddeployqt"
if not exist "%ANDROID_DEPLOY_QT%" (
    echo ERROR: androiddeployqt was not found:
    echo   %QT_HOST_PATH%\bin
    goto fail
)

set "DEPLOYMENT_JSON=%BUILD_DIR%\android-Peare-deployment-settings.json"
set "ANDROID_OUTPUT=%BUILD_DIR%\android-build"
set "APP_LIBRARY=%BUILD_DIR%\libPeare_%ANDROID_ABI%.so"
set "APP_LIBRARY_DIR=%ANDROID_OUTPUT%\libs\%ANDROID_ABI%"
set "APK_FILE=%ANDROID_OUTPUT%\Peare.apk"
set "ANDROID_DEPLOY_MODE=debug-signed"
if "%PEARE_ANDROID_RELEASE%"=="1" set "ANDROID_DEPLOY_MODE=release"

if not exist "%DEPLOYMENT_JSON%" (
    echo ERROR: Android deployment settings were not generated:
    echo   %DEPLOYMENT_JSON%
    goto fail
)
if not exist "%APP_LIBRARY%" (
    echo ERROR: Android application library was not generated:
    echo   %APP_LIBRARY%
    goto fail
)

if not exist "%APP_LIBRARY_DIR%" mkdir "%APP_LIBRARY_DIR%"
copy /y "%APP_LIBRARY%" "%APP_LIBRARY_DIR%\" >nul
if errorlevel 1 goto fail

echo Android package: %ANDROID_DEPLOY_MODE%

if "%PEARE_ANDROID_RELEASE%"=="1" (
    if not defined QT_ANDROID_KEYSTORE_PATH (
        echo ERROR: PEARE_ANDROID_RELEASE=1 requires QT_ANDROID_KEYSTORE_PATH.
        goto fail
    )
    if not defined QT_ANDROID_KEYSTORE_ALIAS (
        echo ERROR: PEARE_ANDROID_RELEASE=1 requires QT_ANDROID_KEYSTORE_ALIAS.
        goto fail
    )
    "%ANDROID_DEPLOY_QT%" ^
        --input "%DEPLOYMENT_JSON%" ^
        --output "%ANDROID_OUTPUT%" ^
        --apk "%APK_FILE%" ^
        --builddir "%BUILD_DIR%" ^
        --android-platform "%PEARE_ANDROID_PLATFORM%" ^
        --jdk "%JAVA_HOME%" ^
        --release ^
        --sign "%QT_ANDROID_KEYSTORE_PATH%" "%QT_ANDROID_KEYSTORE_ALIAS%"
) else (
    rem Omitting --release makes androiddeployqt produce an installable APK
    rem signed with the standard Android debug key.
    "%ANDROID_DEPLOY_QT%" ^
        --input "%DEPLOYMENT_JSON%" ^
        --output "%ANDROID_OUTPUT%" ^
        --apk "%APK_FILE%" ^
        --builddir "%BUILD_DIR%" ^
        --android-platform "%PEARE_ANDROID_PLATFORM%" ^
        --jdk "%JAVA_HOME%"
)
if errorlevel 1 (
    echo.
    echo === ANDROID PACKAGING FAILED ===
    goto fail
)

if not exist "%APK_FILE%" (
    echo ERROR: Packaging completed without producing the APK:
    echo   %APK_FILE%
    goto fail
)

echo.
echo === ANDROID BUILD SUCCEEDED ===
echo Build directory: %CD%\%BUILD_DIR%
echo APK: %CD%\%APK_FILE%
echo Package mode: %ANDROID_DEPLOY_MODE%
if defined PEARE_PUSHD popd
echo.
pause
exit /b 0

:fail
echo.
echo === BUILD FAILED ===
echo Check the error message above.
echo.
if defined PEARE_PUSHD popd
pause
exit /b 1

:try_jdk17
set "JDK_PROBE_VERSION="
set "JDK_PROBE_MAJOR="
set "JDK_PROBE_FILE=%TEMP%\peare-java-version-!RANDOM!-!RANDOM!.txt"
if not exist "%~1\bin\java.exe" goto :eof
"%~1\bin\java.exe" -version >"!JDK_PROBE_FILE!" 2>&1
for /f "usebackq tokens=3" %%V in ("!JDK_PROBE_FILE!") do if not defined JDK_PROBE_VERSION set "JDK_PROBE_VERSION=%%~V"
del /q "!JDK_PROBE_FILE!" >nul 2>nul
for /f "tokens=1 delims=." %%M in ("!JDK_PROBE_VERSION!") do set "JDK_PROBE_MAJOR=%%~M"
if "!JDK_PROBE_MAJOR!"=="17" set "JAVA_17_HOME=%~1"
goto :eof
