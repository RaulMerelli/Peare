import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from conftest import ROOT
from run_gui_preview_tests import valid_png
from peare_opener import _Context, _HANDLE


FIXTURES = ROOT / "tests" / "fixtures" / "android-sparse"
SPARSE = FIXTURES / "test_file.bin.sparse"
RAW = FIXTURES / "test_file.bin"
RAW_SHA256 = "132332c4dd311b60b4d882425f1b4e7b0a6b06976fbd4665cc11f7187599195a"
SPARSE_SHA256 = "4dd6ad319ded0d346440e0a63a73dba3e25f8989b0fb9ac6072c0ae6b54deed0"


def test_android_sparse_random_access_matches_aosp_raw_reference(peare_lib):
    assert hashlib.sha256(SPARSE.read_bytes()).hexdigest() == SPARSE_SHA256
    expected = RAW.read_bytes()
    assert hashlib.sha256(expected).hexdigest() == RAW_SHA256
    with peare_lib.open_file(str(SPARSE)) as opener:
        assert opener.container_format() == "ANDROID_SPARSE"
        tree = opener.normalized_tree(recurse_containers=False)
        assert len(tree) == 1
        assert tree[0]["path"] == "Whole disk"
        assert tree[0]["size"] == len(expected)
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(
            opener._h, 0, 0, resource) == 0
        try:
            context = _Context()
            assert opener._L.peare_resource_get_context(resource, context) == 0
            size = int(context.data_size)
            opener._L.peare_resource_context_free(context)
            decoded = bytearray()
            for offset in range(0, size, 4096):
                part = opener._lib.read_resource(resource, offset, min(4096, size - offset))
                assert part
                decoded.extend(part)
            assert bytes(decoded) == expected
        finally:
            opener._L.peare_resource_destroy(resource)


def test_android_sparse_gui_preview_matches_aosp_raw_reference(peare_gui_path):
    assert hashlib.sha256(SPARSE.read_bytes()).hexdigest() == SPARSE_SHA256
    output = ROOT / "tests" / "output" / "android-sparse" / "whole-disk.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    plugin_name = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    plugin_dir = Path(environment.get("QT_QPA_PLATFORM_PLUGIN_PATH", ""))
    if not (plugin_dir / plugin_name).is_file():
        candidates = [ROOT / "build-windows" / "test-plugins" / "platforms",
                     peare_gui_path.parent / "platforms"]
        plugin_dir = next((path for path in candidates
                           if (path / plugin_name).is_file()), candidates[-1])
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run([
        str(peare_gui_path), "--preview-resource", str(SPARSE),
        "Whole disk", str(output),
    ], capture_output=True, text=True, env=environment, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"]
    assert inspection["size"] == RAW.stat().st_size
    assert inspection["sha256"] == RAW_SHA256
    assert not inspection.get("duplicate_paths")
    assert valid_png(output)
