import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

import pytest

from conftest import ROOT
from peare_opener import _Blob


SAMPLE = Path(os.environ.get("PEARE_ANDROID_SAMPLE", ""))
V31_SAMPLE = Path(os.environ.get("PEARE_ANDROID_V31_SAMPLE", ""))
SAMPLE_SHA256 = "985f5181d48bb6bafd54083a048b391271e0ab28385881cc41294fb01a222762"
MANIFEST_SHA256 = "57354b530875b886fa0c82d596a4ed1641c2ae09d5c8db4e4897e88c6be21acd"
V31_SAMPLE_SHA256 = "0a170dd6ff7e069274635d1d9a857a3c9cbd3e326bf0f4034cf969680613f401"


class _BlobArray(C.Structure):
    _fields_ = [("items", C.POINTER(_Blob)), ("count", C.c_size_t)]


def _manifest(sample):
    with zipfile.ZipFile(sample) as apk:
        assert apk.testzip() is None
        names = apk.namelist()
        assert len(names) == len(set(names))
        return apk.read("AndroidManifest.xml"), names


def test_fdroid_apk_reader_and_manifest_decoder(peare_lib, peare_decoder_path):
    if not SAMPLE.is_file():
        pytest.skip("Set PEARE_ANDROID_SAMPLE to an external signed Android APK")
    assert hashlib.sha256(SAMPLE.read_bytes()).hexdigest() == SAMPLE_SHA256
    manifest, names = _manifest(SAMPLE)
    assert hashlib.sha256(manifest).hexdigest() == MANIFEST_SHA256
    assert len(manifest) == 48248

    with peare_lib.open_file(str(SAMPLE)) as opener:
        assert opener.container_format() == "ANDROID_APK"
        rows = opener.normalized_tree(recurse_containers=False, include_resource_type=True)
        row = next(row for row in rows if row["path"] == "AndroidManifest.xml")
        assert row["resource_type"] == "APK_FILE"
        assert row["size"] == len(manifest)
        assert row["sha256"] == MANIFEST_SHA256
        assert len(rows) == len(names)

        description = _Blob()
        peare_lib._lib.peare_opener_get_description.argtypes = [C.c_void_p, C.POINTER(_Blob)]
        peare_lib._lib.peare_opener_get_description.restype = C.c_int
        peare_lib._lib.peare_blob_free.argtypes = [C.POINTER(_Blob)]
        assert peare_lib._lib.peare_opener_get_description(
            opener._h, C.byref(description)) == 0
        try:
            summary = description.to_bytes().decode("utf-8")
        finally:
            peare_lib._lib.peare_blob_free(C.byref(description))
        assert "Android APK" in summary
        assert "v2 (1 signer)" in summary
        assert "v3 (1 signer)" in summary
        assert "43238d512c1e5eb2d6569f4a3afbf5523418b82e0a3ed1552770abb9a9c9ccab" in summary
        assert "CN=Ciaran Gultnieks" in summary

    decoder = C.CDLL(str(peare_decoder_path))
    decoder.peare_decode_texts.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        C.POINTER(_BlobArray),
    ]
    decoder.peare_decode_texts.restype = C.c_int
    decoder.peare_blob_array_free.argtypes = [C.POINTER(_BlobArray)]
    data = (C.c_uint8 * len(manifest)).from_buffer_copy(manifest)
    texts = _BlobArray()
    assert decoder.peare_decode_texts(data, len(manifest), None, b"APK_FILE",
                                      C.byref(texts)) == 0
    try:
        decoded = b"\n".join(C.string_at(texts.items[i].bytes,
                            texts.items[i].length) for i in range(texts.count)).decode("utf-8")
    finally:
        decoder.peare_blob_array_free(C.byref(texts))
    assert '<manifest xmlns:android="http://schemas.android.com/apk/res/android"' in decoded
    assert 'package="org.fdroid.fdroid"' in decoded
    assert 'android:versionName="1.23.2"' in decoded
    assert 'android:minSdkVersion="23"' in decoded
    assert 'android.permission.INTERNET' in decoded


def test_fdroid_apk_gui_previews_binary_manifest(peare_gui_path):
    if not SAMPLE.is_file():
        pytest.skip("Set PEARE_ANDROID_SAMPLE to an external signed Android APK")
    manifest, _ = _manifest(SAMPLE)
    output_dir = ROOT / "tests" / "output" / "android-apk-gui"
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = output_dir / "manifest.json"
    cases.write_text(json.dumps([{
        "name": "fdroid-android-binary-manifest",
        "source": str(SAMPLE),
        "tree_path": "AndroidManifest.xml",
        "expected_size": len(manifest),
        "expected_sha256": hashlib.sha256(manifest).hexdigest(),
        "expect_no_images": True,
    }]), encoding="utf-8")
    result = subprocess.run([
        sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
        "--exe", str(peare_gui_path), "--output-dir", str(output_dir),
        "--manifest", str(cases), "--report", str(output_dir / "report.json")],
        capture_output=True, text=True, timeout=120, check=False,
        env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 total, 1 passed, 0 failed." in result.stdout


def test_apk_v31_rotation_metadata(peare_lib):
    if not V31_SAMPLE.is_file():
        pytest.skip("Set PEARE_ANDROID_V31_SAMPLE to the external apksigner v3.1 sample")
    assert hashlib.sha256(V31_SAMPLE.read_bytes()).hexdigest() == V31_SAMPLE_SHA256
    with peare_lib.open_file(str(V31_SAMPLE)) as opener:
        assert opener.container_format() == "ANDROID_APK"
        _manifest_bytes, names = _manifest(V31_SAMPLE)
        assert "AndroidManifest.xml" in names
        rows = opener.normalized_tree(recurse_containers=False, include_resource_type=True)
        manifest_row = next(row for row in rows if row["path"] == "AndroidManifest.xml")
        assert manifest_row["resource_type"] == "APK_FILE"
        description = _Blob()
        peare_lib._lib.peare_opener_get_description.argtypes = [C.c_void_p, C.POINTER(_Blob)]
        peare_lib._lib.peare_opener_get_description.restype = C.c_int
        peare_lib._lib.peare_blob_free.argtypes = [C.POINTER(_Blob)]
        assert peare_lib._lib.peare_opener_get_description(
            opener._h, C.byref(description)) == 0
        try:
            summary = description.to_bytes().decode("utf-8")
        finally:
            peare_lib._lib.peare_blob_free(C.byref(description))
        assert "v3.1 (1 signer)" in summary
        assert "CN=Peare Test Rotated" in summary
        assert "8ee2d089f4c83087d6bd3731b888051e18504f3decc04edb8587fcb12a9c52c2" in summary
        assert "SDK 24-32" in summary
        assert "SDK 33-2147483647" in summary
        assert "v3 key lineage: Peare Test Old" in summary
        assert " -> Peare Test Rotated" in summary


def test_apk_v31_gui_previews_manifest(peare_gui_path):
    if not V31_SAMPLE.is_file():
        pytest.skip("Set PEARE_ANDROID_V31_SAMPLE to the external apksigner v3.1 sample")
    manifest, _names = _manifest(V31_SAMPLE)
    output_dir = ROOT / "tests" / "output" / "android-apk-v31-gui"
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = output_dir / "manifest.json"
    cases.write_text(json.dumps([{
        "name": "fdroid-apk-v31-binary-manifest",
        "source": str(V31_SAMPLE), "tree_path": "AndroidManifest.xml",
        "expected_size": len(manifest),
        "expected_sha256": hashlib.sha256(manifest).hexdigest(),
        "expect_no_images": True,
    }]), encoding="utf-8")
    result = subprocess.run([
        sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
        "--exe", str(peare_gui_path), "--output-dir", str(output_dir),
        "--manifest", str(cases), "--report", str(output_dir / "report.json")],
        capture_output=True, text=True, timeout=120, check=False,
        env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 total, 1 passed, 0 failed." in result.stdout
