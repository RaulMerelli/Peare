param(
    [ValidateSet("full", "precommit", "prepush")]
    [string]$Mode = "full"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $root
$tmpRoot = Join-Path $root "tmp"
$testOutput = Join-Path $root "tests\output"
$receipt = Join-Path $tmpRoot "local-gate-pass.txt"
$limitBytes = [long]10000000000

function Invoke-Git {
    param([string[]]$GitArgs)
    $output = & git @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed." }
    return ($output | Out-String).Trim()
}

function Get-TempBytes {
    if (-not (Test-Path $tmpRoot)) { return 0L }
    $measure = Get-ChildItem -LiteralPath $tmpRoot -File -Recurse -Force |
        Measure-Object -Property Length -Sum
    if ($null -eq $measure.Sum) { return 0L }
    return [long]$measure.Sum
}

function Get-TestOutputBytes {
    if (-not (Test-Path $testOutput)) { return 0L }
    $measure = Get-ChildItem -LiteralPath $testOutput -File -Recurse -Force |
        Measure-Object -Property Length -Sum
    if ($null -eq $measure.Sum) { return 0L }
    return [long]$measure.Sum
}

function Assert-WorkingTreeMatchesIndex {
    & git diff --quiet
    if ($LASTEXITCODE -ne 0) {
        throw "Tracked working-tree changes must be staged before the local gate."
    }
}

function Invoke-LoggedTestRun {
    param([string]$Python, [string[]]$Arguments, [string]$LogName)
    $stdout = Join-Path $tmpRoot "$LogName.stdout.log"
    $stderr = Join-Path $tmpRoot "$LogName.stderr.log"
    $process = Start-Process -FilePath $Python -ArgumentList $Arguments `
        -WorkingDirectory $root -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    while (-not $process.HasExited) {
        Start-Sleep -Seconds 5
        $process.Refresh()
        $used = Get-TempBytes
        $testUsed = Get-TestOutputBytes
        if ($used -ge $limitBytes -or $testUsed -ge $limitBytes) {
            & taskkill.exe /PID $process.Id /T /F | Out-Null
            throw "Test output reached the 10 GB limit; stopped the test process."
        }
    }
    $process.WaitForExit()
    $process.Refresh()
    $exitCode = $process.ExitCode
    Get-Content -LiteralPath $stdout
    Get-Content -LiteralPath $stderr
    if ($null -eq $exitCode -or $exitCode -ne 0) {
        throw "Test command failed with exit code $exitCode."
    }
}

if ($Mode -eq "prepush") {
    Assert-WorkingTreeMatchesIndex
    $tree = Invoke-Git @("rev-parse", "HEAD^{tree}")
    if (-not (Test-Path $receipt) -or
        (Get-Content -LiteralPath $receipt -Raw).Trim() -ne $tree) {
        throw "HEAD has no successful local-gate receipt. Run the pre-commit gate first."
    }
    exit 0
}

Assert-WorkingTreeMatchesIndex
& git diff --cached --check
if ($LASTEXITCODE -ne 0) { throw "Staged diff has whitespace errors." }
New-Item -ItemType Directory -Path $tmpRoot -Force | Out-Null
$tree = Invoke-Git @("write-tree")
if ($Mode -eq "precommit" -and (Test-Path $receipt) -and
    (Get-Content -LiteralPath $receipt -Raw).Trim() -eq $tree) {
    Write-Output "Local gate already passed for staged tree $tree."
    exit 0
}
if ((Get-TempBytes) -ge $limitBytes) { throw "tmp is already at its 10 GB limit." }

$env:PEARE_NO_PAUSE = "1"
$env:PEARE_EXTERNAL_ORACLES = "0"
$qtWindows = "C:\Qt\5.15.2\msvc2019_64"
$qmakeWindows = Join-Path $qtWindows "bin\qmake.exe"
if (-not (Test-Path $qmakeWindows)) { throw "Qt 5.15.2 MSVC kit is missing: $qtWindows" }
$qtWindowsVersion = & $qmakeWindows -query QT_VERSION
if ($LASTEXITCODE -ne 0 -or $qtWindowsVersion.Trim() -ne "5.15.2") {
    throw "Windows build requires Qt 5.15.2. Found: $qtWindowsVersion"
}
$cmake = Get-Command cmake.exe -ErrorAction Stop
$ninja = Get-Command ninja.exe -ErrorAction Stop
$cmakeVersion = (& $cmake.Source --version | Select-Object -First 1) -replace '^cmake version ', ''
$ninjaVersion = (& $ninja.Source --version | Select-Object -First 1).Trim()
if ([version]$cmakeVersion -lt [version]'3.22' -or
    [version]$cmakeVersion -ge [version]'5.0' -or
    [version]$ninjaVersion -lt [version]'1.10' -or
    [version]$ninjaVersion -ge [version]'2.0') {
    throw "Unsupported build tools. Required: CMake 3.22.x-4.x and Ninja 1.x. Found: $cmakeVersion, $ninjaVersion."
}

$androidSdk = Join-Path $env:LOCALAPPDATA "Android\Sdk"
$androidNdk = Join-Path $androidSdk "ndk\26.1.10909125"
$androidNdkProperties = Join-Path $androidNdk "source.properties"
if (-not (Test-Path $androidNdkProperties) -or
    -not (Select-String -LiteralPath $androidNdkProperties `
        -Pattern '^Pkg\.Revision\s*=\s*26\.1\.10909125\s*$' -Quiet)) {
    throw "Android CI NDK 26.1.10909125 is not installed."
}
$androidPlatform = Join-Path $androidSdk "platforms\android-34\android.jar"
if (-not (Test-Path $androidPlatform)) { throw "Android API 34 is not installed." }
$androidAapt = Join-Path $androidSdk "build-tools\34.0.0\aapt.exe"
if (-not (Test-Path $androidAapt)) { throw "Android Build Tools 34.0.0 are not installed." }
$qtAndroid = "C:\Qt\6.7.3\android_arm64_v8a"
if (-not (Test-Path (Join-Path $qtAndroid "bin\qt-cmake.bat"))) {
    throw "Qt 6.7.3 Android kit is missing: $qtAndroid"
}
$qtHostPath = $null
foreach ($kit in Get-ChildItem "C:\Qt\6.7.3" -Directory -ErrorAction SilentlyContinue) {
    $qmake = Join-Path $kit.FullName "bin\qmake.exe"
    if (-not (Test-Path $qmake) -or
        -not (Test-Path (Join-Path $kit.FullName "bin\moc.exe")) -or
        -not (Test-Path (Join-Path $kit.FullName "bin\androiddeployqt.exe"))) {
        continue
    }
    $version = & $qmake -query QT_VERSION
    if ($LASTEXITCODE -eq 0 -and $version.Trim() -eq "6.7.3") {
        $qtHostPath = $kit.FullName
        break
    }
}
if (-not $qtHostPath) {
    throw "Qt 6.7.3 desktop host tools are missing under C:\Qt\6.7.3."
}
$env:QT_HOST_PATH = $qtHostPath
$python = (Get-Command py.exe -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source
$pythonVersion = & $python -3.12 --version
if ($LASTEXITCODE -ne 0 -or $pythonVersion -notmatch '^Python 3\.12\.') {
    throw "The test gate requires Python 3.12. Found: $pythonVersion"
}
$pythonExe = (& $python -3.12 -c "import sys; print(sys.executable)").Trim()
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $pythonExe)) {
    throw "Could not resolve the Python 3.12 interpreter executable."
}
$includeCaseCheck = Join-Path $root "tests\check_include_case.py"
& $python -3.12 $includeCaseCheck
if ($LASTEXITCODE -ne 0) { throw "Source-relative include case check failed." }
$jdk17 = Get-ChildItem "C:\Program Files\Eclipse Adoptium\jdk-17*" `
    -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $jdk17 -or -not (Test-Path (Join-Path $jdk17.FullName "bin\java.exe"))) {
    throw "A local JDK 17 installation is required for the Android build."
}
$env:JAVA_HOME = $jdk17.FullName
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
$jdkRelease = Join-Path $env:JAVA_HOME "release"
if (-not (Test-Path $jdkRelease) -or -not (Select-String -LiteralPath $jdkRelease `
        -Pattern '^JAVA_VERSION="17\.' -Quiet)) {
    throw "Android packaging requires JDK 17: $env:JAVA_HOME"
}
$windowsBuild = Join-Path $root "tools\build\build_windows.bat"
& cmd.exe /d /c "call `"$windowsBuild`""
if ($LASTEXITCODE -ne 0) { throw "Official Windows build failed." }

$windowsCache = Join-Path $root "build-windows\CMakeCache.txt"
if (-not (Select-String -LiteralPath $windowsCache -SimpleMatch `
        "PEARE_CXX_STANDARD:STRING=11" -Quiet)) {
    throw "Windows build cache does not select the required C++11 baseline."
}
foreach ($artifact in @(
    "Peare.exe", "PeareOpener.dll", "PeareDecoder.dll", "PeareWriter.dll",
    "test-plugins\platforms\qoffscreen.dll")) {
    if (-not (Test-Path (Join-Path $root "build-windows\Release\$artifact"))) {
        if ($artifact -notlike "test-plugins*" -or -not (Test-Path `
            (Join-Path $root "build-windows\$artifact"))) {
            throw "Windows build output is missing: $artifact"
        }
    }
}

$env:ANDROID_NDK_ROOT = $androidNdk
$env:ANDROID_SDK_ROOT = $androidSdk
$env:PEARE_ANDROID_PLATFORM = "android-34"
$androidBuild = Join-Path $root "tools\build\build_android.cmd"
& cmd.exe /d /c "call `"$androidBuild`""
if ($LASTEXITCODE -ne 0) { throw "Official Android build or APK packaging failed." }
$apk = Join-Path $root `
    "build-android\android-build\build\outputs\apk\debug\android-build-debug.apk"
if (-not (Test-Path $apk)) { throw "Android APK is missing: $apk" }

$env:PEARE_OPENER_LIB = Join-Path $root "build-windows\Release\PeareOpener.dll"
$env:PEARE_WRITER_LIB = Join-Path $root "build-windows\Release\PeareWriter.dll"
$env:PEARE_GUI_EXE = Join-Path $root "build-windows\Release\Peare.exe"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $root `
    "build-windows\test-plugins\platforms"
$env:PEARE_ANDROID_APK = $null
Invoke-LoggedTestRun -Python $pythonExe `
    -Arguments @("tests/run_tests.py", "-q") -LogName "local-gate-tests"
$env:PEARE_ANDROID_APK = $apk
Invoke-LoggedTestRun -Python $pythonExe `
    -Arguments @("tests/run_tests.py", "-q", "tests/test_android_package.py") `
    -LogName "local-gate-android-package"

if ((Get-TempBytes) -ge $limitBytes -or
    (Get-TestOutputBytes) -ge $limitBytes) {
    throw "Local gate output exceeded its 10 GB limit."
}
$tree = Invoke-Git @("write-tree")
Set-Content -LiteralPath $receipt -Value $tree -Encoding ascii
Write-Output "Local gate passed for tree $tree."
