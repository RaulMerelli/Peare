import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import ctypes as C
import zipfile
import xml.etree.ElementTree as ET

import pytest

from conftest import ROOT
from peare_opener import _Blob


SAMPLE = Path(os.environ.get("PEARE_TIA_ZAP_SAMPLE", ""))
SAMPLE_SHA256 = "4a7f37a043f51c3378185334efb550af9963d4c41a1fcead8da899f9363e224f"
GENERIC_ZIP = ROOT / "tests" / "formats" / "zipx" / "fixtures" / "README-DCL-ascii-1024.zip"


def _description(opener):
    blob = _Blob()
    opener._L.peare_opener_get_description.argtypes = [C.c_void_p, C.POINTER(_Blob)]
    opener._L.peare_opener_get_description.restype = C.c_int
    assert opener._L.peare_opener_get_description(opener._h, C.byref(blob)) == 0
    try:
        return blob.to_bytes().decode("utf-8", "replace")
    finally:
        opener._L.peare_blob_free(C.byref(blob))


def test_zap17_is_read_as_a_generic_zip_archive(peare_lib):
    if not SAMPLE.is_file():
        pytest.skip("Set PEARE_TIA_ZAP_SAMPLE to the external .zap17 ZIP sample")
    assert hashlib.sha256(SAMPLE.read_bytes()).hexdigest() == SAMPLE_SHA256
    with zipfile.ZipFile(SAMPLE) as archive:
        infos = archive.infolist()
        assert archive.testzip() is None
        names = [info.filename.rstrip("/") for info in infos]
        assert len(names) == len(set(names))
        manifest_name = "RuntimePersisteny_Demo_V17.ap17"
        manifest = ET.fromstring(archive.read(manifest_name))
        assert manifest.attrib["Type"] == "Project"
        assert manifest.attrib["ProjectCompatibilityVersion"] == "17.0.0.0"
        expected = {info.filename.rstrip("/"): info.file_size for info in infos}

    with peare_lib.open_file(str(SAMPLE)) as opener:
        assert opener.container_format() == "ZIP"
        assert _description(opener) == "ZIP archive"
        actual_rows = opener.normalized_tree(recurse_containers=False)
        actual = {row["path"]: row["size"] for row in actual_rows}
        assert len(actual_rows) == 120
        assert actual == expected
        assert "IM/HMI/I/0/Saved/general/cds.rdf" in actual
        assert manifest_name in actual


def test_regular_zip_description_is_unchanged(peare_lib):
    with peare_lib.open_file(str(GENERIC_ZIP)) as opener:
        assert opener.container_format() == "ZIP"
        assert _description(opener) == "ZIP archive"


def test_tia_zap17_gui_previews_project_manifest(peare_gui_path):
    if not SAMPLE.is_file():
        pytest.skip("Set PEARE_TIA_ZAP_SAMPLE to the external .zap17 ZIP sample")
    with zipfile.ZipFile(SAMPLE) as archive:
        manifest = archive.read("RuntimePersisteny_Demo_V17.ap17")
    output_dir = ROOT / "tests" / "output" / "tia-zap17-gui"
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = output_dir / "manifest.json"
    cases.write_text(json.dumps([{
        "name": "tia-v17-project-manifest",
        "source": str(SAMPLE),
        "tree_path": "RuntimePersisteny_Demo_V17.ap17",
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
