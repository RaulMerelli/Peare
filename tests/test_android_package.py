"""Check the architecture of native libraries in the Android APK."""

import os
from pathlib import Path
import struct
from zipfile import ZipFile

import pytest


ROOT = Path(__file__).resolve().parents[1]
APK = Path(os.environ.get(
    "PEARE_ANDROID_APK", ROOT / "build-android/android-build/Peare.apk"))
REQUIRED_LIBRARIES = {
    "libPeareDecoder.so",
    "libPeareOpener.so",
    "libPeareWriter.so",
}


def test_android_apk_contains_arm64_peare_libraries():
    if not APK.is_file():
        pytest.skip(f"Android APK is not available: {APK}")

    with ZipFile(APK) as apk:
        names = {
            name for name in apk.namelist()
            if name.startswith("lib/arm64-v8a/") and name.endswith(".so")
        }
        present = {Path(name).name for name in names}
        assert REQUIRED_LIBRARIES <= present
        assert names

        for name in sorted(names):
            with apk.open(name) as library:
                header = library.read(20)
            assert header[:4] == b"\x7fELF", name
            assert header[4] == 2, name
            assert header[5] == 1, name
            assert struct.unpack_from("<H", header, 18)[0] == 183, name
