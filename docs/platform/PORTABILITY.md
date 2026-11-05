# Platform status

Peare has four build targets: the GUI, PeareOpener, PeareDecoder, and
PeareWriter. A build profile does not prove that a release package or device
works. The table separates build status from runtime status.

| Platform | Build status | Runtime or package status |
|---|---|---|
| Windows | Verified with MSVC and Qt 5.15 | Primary supported target |
| Linux x64 | Qt 5.15 build profile | Package status is not verified |
| Linux ARM64 | Qt 5.15 cross-build profile | Not run on an ARM64 device |
| macOS | No build profile | No app bundle or signing workflow |
| Android ARM64 | Qt 6 build profile | Device testing remains open |
| OS/2 | Source code may be portable | No supported Qt toolchain or build profile |

## Source portability

- Resource decoding uses Qt and fixed-width integer types, not Win32 resource APIs.
- GUI icons have a Qt resource fallback. The Windows icon paths are not required to compile.
- `resources/peare.rc` is built only on Windows.
- XEX AES-128-CBC uses portable internal C++ code.
- Public C ABI exports use platform-specific visibility attributes.
- The LZX decoder uses C++11 and builds with PeareOpener.

## Android profile

`tools/build/build_android.cmd` uses a Qt 6 Android kit, Ninja, and `androiddeployqt`.
It builds the GUI and all three libraries. By default, it creates a debug-signed
APK. Device execution and AluminiumOS integration are not verified.

Qt 6 builds need the Qt Core5Compat module for `QTextCodec`.
Install it for both the host Qt kit and the Android target kit.
Qt 5 provides `QTextCodec` through QtCore.

## Work for broader support

1. Add Linux x64, Linux ARM64, and macOS CI builds.
2. Add macOS app metadata, signing, and deployment rules.
3. Test the Android APK on target devices and refine touch input.
4. Add an OS/2 toolchain file only after a maintained Qt and compiler toolchain exists.
