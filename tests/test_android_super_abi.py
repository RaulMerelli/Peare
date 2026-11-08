import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from conftest import ROOT
from peare_opener import _Context, _HANDLE
from run_gui_preview_tests import valid_png


FIXTURES = ROOT / "tests" / "fixtures" / "android-super"
SAMPLE = FIXTURES / "super.img"
EXPECTED = FIXTURES / "system_a.img"
EXPECTED_SHA256 = "d208f0efde13c321ca4e0fa1480d71edaf6da5529f15ca052c26d16edc965d3b"
EMPTY_SAMPLE_SHA256 = "4fbee3bbb59e157eab4b8d8e3b49f66746c906ce57929b1a225f2e5afdd0d739"


def _read_partition(opener, name):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, resource) == 0
            try:
                context = _Context()
                assert opener._L.peare_resource_get_context(resource, context) == 0
                actual_name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                size = int(context.data_size)
                opener._L.peare_resource_context_free(context)
                if actual_name != name:
                    continue
                data = bytearray()
                offset = 0
                while offset < size:
                    chunk = opener._lib.read_resource(
                        resource, offset, min(4096, size - offset))
                    assert chunk
                    data.extend(chunk)
                    offset += len(chunk)
                return bytes(data)
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"Logical partition not found: {name}")


def _require_fixture():
    assert hashlib.sha256(SAMPLE.read_bytes()).hexdigest() == (
        "4a81fa41d550435c739cb2d9ff8547bb00194d84513f5f1c1122c345791f83c6")
    assert hashlib.sha256(EXPECTED.read_bytes()).hexdigest() == EXPECTED_SHA256


def test_android_super_abi_matches_lputil_partition(peare_lib):
    _require_fixture()
    with peare_lib.open_file(str(SAMPLE)) as opener:
        assert opener.container_format() == "ANDROID_SUPER"
        actual = _read_partition(opener, "system_a")
    assert actual == EXPECTED.read_bytes()
    assert len(actual) == 8192


def test_android_super_empty_metadata_image(peare_lib):
    empty_sample = FIXTURES / "super_empty.img"
    assert hashlib.sha256(empty_sample.read_bytes()).hexdigest() == EMPTY_SAMPLE_SHA256
    with peare_lib.open_file(str(empty_sample)) as opener:
        assert opener.container_format() == "ANDROID_SUPER"
        assert _read_partition(opener, "system_a") == b""


def test_android_super_gui_preview_matches_lputil_partition(peare_gui_path):
    _require_fixture()
    output = ROOT / "tests" / "output" / "android-super" / "system_a.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    plugin_name = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    override = environment.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    plugin_dir = Path(override) if override else None
    if plugin_dir is None or not (plugin_dir / plugin_name).is_file():
        candidates = [
            ROOT / "build-windows" / "test-plugins" / "platforms",
            peare_gui_path.parent / "platforms",
        ]
        plugin_dir = next((path for path in candidates
                           if (path / plugin_name).is_file()), candidates[-1])
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run([
        str(peare_gui_path), "--preview-resource", str(SAMPLE), "system_a", str(output),
    ], capture_output=True, text=True, env=environment, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"]
    assert inspection["size"] == 8192
    assert inspection["sha256"] == EXPECTED_SHA256
    assert not inspection.get("duplicate_paths")
    assert valid_png(output)
