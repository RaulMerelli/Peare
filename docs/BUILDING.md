# Building Peare

Peare is built with CMake. The repository-provided scripts are the supported entry points and define the toolchain, Qt version, architecture, and deployment steps described below.

## Build matrix

| Target | Script | Qt | Compiler / toolchain | C++ mode | Default output |
|---|---|---|---|---|---|
| Linux x86-64 | `build_linux.sh --arch x64` | Qt Base 5.15.2 | GCC + Ninja | C++11 | `build-linux-x64/Peare` |
| Linux ARM64 | `build_linux.sh --arch arm64` | Qt Base 5.15.2 | AArch64 GNU cross-toolchain + Ninja | C++11 | `build-linux-arm64/Peare` |
| Windows x64 | `build_windows.bat` | Qt 5.15.2 MSVC 2019 x64 | MSVC x64 | C++11 | `build-windows\Release\Peare.exe` |
| Android ARM64 | `build_android.cmd` | Qt 6.7.3 Android ARM64 by default | Android NDK + Ninja | CMake `AUTO` (C++17 with Qt 6) | `build-android\android-build` |

CMake selects Qt 5 for Windows and Linux, and Qt 6 for Android and macOS. The repository currently contains no macOS build script.

## Linux

The Linux workflow is designed for an Ubuntu 20.04 x86-64 host. It does not use Docker or Ubuntu's Qt 5.12 packages. When necessary, it builds Qt Base 5.15.2 from the official source archive.

### One-time host setup

```sh
chmod +x setup_linux_host.sh build_linux.sh cmake/linux/build_qt515.sh
./setup_linux_host.sh
```

`setup_linux_host.sh` enables Ubuntu's `universe` repository and installs the native build tools, Ninja, CMake, the AArch64 cross-compiler, development packages required by Qt, and utilities used to create the ARM64 sysroot.

The scripts use these default locations:

```text
~/.local/opt/Qt/5.15.2/gcc_64
~/.local/opt/Qt/5.15.2/gcc_arm64
~/.local/opt/Qt/5.15.2/gcc_arm64_host
~/.cache/peare/sysroots/ubuntu-focal-arm64
~/.cache/peare/qt-5.15.2
```

The ARM64 sysroot is assembled locally from Ubuntu Focal ARM64 packages; those packages are not installed into the host system.

### Native x86-64 build

```sh
./build_linux.sh --arch x64 --clean
```

Run the resulting application after a successful native build:

```sh
./build_linux.sh --arch x64 --clean --run
```

### ARM64 cross-build

```sh
./build_linux.sh --arch arm64 --clean
```

This uses `cmake/toolchains/linux-arm64-gnu.cmake`, the ARM64 Qt libraries, and x86-64 host versions of Qt tools such as `moc`, `rcc`, `uic`, and `qmake`. The script verifies that the generated executable is an AArch64 ELF file. It does not emulate or run that executable on the x86-64 host.

### Build both Linux architectures

```sh
./build_linux.sh --arch all --clean
```

`--run` can only be used with one architecture.

### Rebuild Qt

```sh
./build_linux.sh --arch x64 --rebuild-qt
./build_linux.sh --arch arm64 --rebuild-qt
```

The Qt source download is cached and its SHA-256 digest is checked before extraction.

### Linux options and environment variables

```text
--arch x64|arm64|all   target architecture; defaults to the host architecture
--clean                remove the selected Peare build directory first
--run                  run Peare after a successful single-architecture build
--rebuild-qt           force rebuilding the selected Qt installation

BUILD_TYPE             Release by default
JOBS                   Peare build parallelism; defaults to nproc
QT_BUILD_JOBS          Qt build parallelism
PEARE_QT_INSTALL_BASE  default: ~/.local/opt/Qt/5.15.2
QT5_ROOT_X64           existing Qt 5.15.2 x86-64 installation
QT5_ROOT_ARM64         existing Qt 5.15.2 ARM64 installation
QT5_HOST_ROOT_ARM64    x86-64 Qt host tools for the ARM64 build
PEARE_ARM64_SYSROOT    ARM64 Ubuntu Focal sysroot
```

Useful checks:

```sh
~/.local/opt/Qt/5.15.2/gcc_64/bin/qmake -v
file build-linux-x64/Peare
file build-linux-arm64/Peare
```

## Windows x64

`build_windows.bat` expects:

- Qt 5.15.2 MSVC 2019 x64 at `C:\Qt\5.15.2\msvc2019_64`;
- Visual Studio with the C++ x64 toolchain;
- CMake available in `PATH`;
- `vswhere.exe` in the standard Visual Studio Installer location.

Run from a normal Command Prompt:

```bat
build_windows.bat
```

The script locates Visual Studio, runs `vcvars64.bat`, configures `build-windows`, builds the `Release` configuration, and runs `windeployqt` on:

```text
build-windows\Release\Peare.exe
```

To use a different Qt installation or build directory, edit `QT_ROOT` or `BUILD_DIR` near the beginning of `build_windows.bat`.

## Android ARM64

`build_android.cmd` builds on Windows with a Qt 6 Android kit. Its defaults are:

```text
Qt Android kit: C:\Qt\6.7.3\android_arm64_v8a
Build type:     Release
Build directory: build-android
ABI:            arm64-v8a
Android API:    android-34
```

Required components:

- a matching desktop Qt kit, used as `QT_HOST_PATH`;
- Android SDK with platform `android-34`;
- Android NDK, preferably r26b (`26.1.10909125` for the default Qt kit);
- JDK 17;
- Ninja in `PATH`.

Example:

```bat
set QT_HOST_PATH=C:\Qt\6.7.3\msvc2019_64
set ANDROID_SDK_ROOT=%LOCALAPPDATA%\Android\Sdk
set ANDROID_NDK_ROOT=%ANDROID_SDK_ROOT%\ndk\26.1.10909125
build_android.cmd
```

The positional arguments are the Qt Android kit, build type, and build directory:

```bat
build_android.cmd C:\Qt\6.7.3\android_arm64_v8a Release build-android
```

The script removes an existing Android build directory by default. Set the following to reuse it:

```bat
set PEARE_REUSE_ANDROID_BUILD=1
```

For a signed release package, set `PEARE_ANDROID_RELEASE=1` and provide the signing variables required by the script, including `QT_ANDROID_KEYSTORE_PATH` and `QT_ANDROID_KEYSTORE_ALIAS`.

## Direct CMake configuration

The scripts pass the project-specific cache variables below:

```text
PEARE_QT_MAJOR       AUTO, 5, or 6
PEARE_CXX_STANDARD   AUTO, 11, or 17
PEARE_QT_HOST_PATH   host Qt tools for a Qt 5 cross-build
```

Use the scripts unless integrating Peare into another build system: they also validate prerequisites, create or select the correct Qt installation, configure cross-compilation, and perform platform deployment steps.

## Installed targets

A desktop build produces the Peare application and two shared libraries:

```text
Peare
PeareOpener
PeareDecoder
```

On Windows these are `Peare.exe`, `PeareOpener.dll`, and `PeareDecoder.dll`. Platform-specific library prefixes and suffixes are used elsewhere.
