# Platform portability status

This document distinguishes source portability from a verified, packaged application target. The project produces three logical targets: the Qt GUI application, `PeareOpener`, and `PeareDecoder`.

| Platform | Libraries | GUI executable/package | Current limitation | Status |
|---|---|---|---|---|
| Windows | Supported and exercised with MSVC/Qt 5.15 | Supported and exercised | None known for the supported feature set | Verified primary target |
| Linux | CMake and source layout are compatible with Qt 5.15, GCC or Clang | A normal desktop executable is expected | No known parser feature gap; reproducible Ubuntu 20.04/Qt 5.15.2 x64 and ARM64 container profiles are available, while packaging remains unverified | x64 and ARM64 build profiles available; packaging not verified |
| macOS | Reserved for a future Qt 6/Clang profile | A command-style GUI executable is expected; no `.app` bundle or signing workflow is configured | Packaging is not implemented | Plausible, not build-tested |
| Android | Qt 6 profile with both libraries | `qt_add_executable` plus `androiddeployqt` packaging | Device execution and touch-oriented validation remain pending | Build profile available |
| OS/2 | Parsers do not depend on the host OS and the public C ABI has no Windows-only types | Depends on an external Qt 5/CMake/compiler port | Qt 5.15 for OS/2 is not an upstream-supported toolchain, and no CI or packaging exists | Theoretical source target only |

## Source audit

- Windows resource decoding is implemented with Qt and fixed-width integer types rather than Win32 resource APIs.
- The GUI icon defaults contain Windows file paths, but icon loading has a Qt-resource fallback and does not require `shell32.dll` to compile.
- `resources/peare.rc` is added only when `WIN32` is true.
- AES-128-CBC for encrypted XEX files is implemented by internal portable C++ code and has no operating-system cryptography dependency.
- Public C ABI exports use `__declspec` on Windows and default symbol visibility attributes on GCC-compatible compilers.
- The internal LZX decoder is C++11 and is built as part of PeareOpener.

## Work required for full parity

1. Add Linux x64/ARM64 and macOS CI builds.
2. Add `MACOSX_BUNDLE`, application metadata, signing and deployment rules for a distributable macOS app.
3. Validate the existing Qt 6 Android APK profile on target devices and refine touch integration.
4. Treat OS/2 as conditional on a maintained Qt/toolchain port and add a dedicated toolchain file before claiming support.

The current conclusion is therefore: Windows remains fully supported with Qt 5.15; Linux has Qt 5.15 x64 and ARM64 build profiles; macOS is reserved for a future Qt 6 profile; Android uses Qt 6; OS/2 remains only a theoretical target until a usable Qt toolchain is selected and tested.

## Android build profile

The repository now contains `build_android.cmd`, which configures a Qt 6 Android kit from Windows through `qt-cmake.bat`, uses Ninja, builds the Qt Widgets application and its two shared libraries, and invokes `androiddeployqt` directly and creates a debug-signed APK by default so the result is installable without a release keystore. This is the first build profile; device execution and AluminiumOS-specific integration remain to be validated.

## Qt 6 text codec compatibility

Qt 6 builds require the Qt Core5Compat module because Peare still uses `QTextCodec` for legacy Windows and OS/2 code pages. Install the Core5Compat component for both the host Qt kit and the Android target kit. Qt 5 builds use `QTextCodec` directly from QtCore.
