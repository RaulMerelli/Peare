import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

import pytest

from conftest import ROOT
from peare_opener import _Context, _HANDLE, load_library


SAMPLE_DIR = Path(os.environ.get(
    "PEARE_XDBF_SAMPLE_DIR", ROOT / "tests" / "fixtures" / "xdbf"))
NAMESPACE_NAMES = {
    1: "Achievements", 2: "Images", 3: "Settings", 4: "Titles",
    5: "Strings", 6: "Achievement security",
}


def _expected_records(path):
    data = path.read_bytes()
    assert data[:4] == b"XDBF"
    version, entry_capacity, entry_count, free_capacity, free_count = struct.unpack_from(
        ">5I", data, 4)
    assert version == 0x10000
    assert entry_count <= entry_capacity
    assert free_count <= free_capacity
    data_offset = 24 + entry_capacity * 18 + free_capacity * 8
    assert data_offset <= len(data)
    records = {}
    title = ""
    for index in range(entry_count):
        namespace, record_id, offset, size = struct.unpack_from(
            ">HQII", data, 24 + index * 18)
        start = data_offset + offset
        end = start + size
        assert end <= len(data)
        name = f"0x{record_id:016X}"
        namespace_name = NAMESPACE_NAMES.get(namespace, f"Namespace 0x{namespace:04X}")
        payload = data[start:end]
        digest = hashlib.sha256(payload).hexdigest() if payload else ""
        records[f"{namespace_name}/{name}"] = (size, digest)
        if namespace == 5 and record_id == 0x8000:
            title = payload.decode("utf-16-be").rstrip("\0")
    return records, title


def _golden_title(path):
    golden = path.with_suffix(path.suffix + ".json")
    if not golden.is_file():
        return ""
    fields = json.loads(golden.read_text(encoding="utf-8")).get("fields", [])
    for field in fields:
        if field.get("desc", {}).get("name") == "Title":
            return field.get("data", "")
    return ""


def _paths_and_hashes(opener):
    return {row["path"]: (row["size"], row["sha256"])
            for row in opener.normalized_tree(recurse_containers=False)}


def _read_resource(opener, name):
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
                if actual_name == name:
                    data = bytearray()
                    for offset in range(0, size, 65536):
                        chunk = opener._lib.read_resource(resource, offset,
                                                          min(65536, size - offset))
                        assert chunk
                        data.extend(chunk)
                    return bytes(data)
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"XDBF resource not found: {name}")


def test_xdbf_abi_reads_independent_gpd_corpus(peare_lib):
    if not SAMPLE_DIR.is_dir():
        pytest.skip("Set PEARE_XDBF_SAMPLE_DIR to the external XDBF/GPD corpus")
    samples = sorted(SAMPLE_DIR.glob("*.gpd"))
    assert len(samples) == 19
    for sample in samples:
        expected, title = _expected_records(sample)
        with peare_lib.open_file(str(sample)) as opener:
            assert opener.container_format() == "XDBF", sample.name
            actual = _paths_and_hashes(opener)
            assert actual["Metadata/summary.txt"][0] > 0
            summary = next(row for row in opener.normalized_tree(recurse_containers=False)
                           if row["path"] == "Metadata/summary.txt")
            assert summary["sha256"]
            summary_text = _read_resource(opener, "summary.txt").decode("utf-8")
            assert f"Records: {len(expected)}" in summary_text
            golden_title = _golden_title(sample)
            if golden_title:
                assert title == golden_title, sample.name
                assert f"Title: {golden_title}" in summary_text
            assert {path: actual[path] for path in expected} == expected, sample.name
            assert len(actual) == len(expected) + 1, sample.name


def test_xdbf_gui_preview_shows_metadata_and_image(peare_gui_path):
    samples = sorted(SAMPLE_DIR.glob("*.gpd")) if SAMPLE_DIR.is_dir() else []
    if not samples:
        pytest.skip("Set PEARE_XDBF_SAMPLE_DIR to the external XDBF/GPD corpus")
    assert len(samples) == 19
    sample = SAMPLE_DIR / "534507D6.gpd"
    records, _ = _expected_records(sample)
    image_path = "Images/0x0000000000008000"
    expected_size, expected_hash = records[image_path]
    output_dir = ROOT / "tests" / "output" / "xdbf-gui"
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = [{"name": f"xdbf-{path.stem}", "source": str(path),
              "tree_path": "Metadata/summary.txt", "expect_no_images": True,
              "require_oracle": False} for path in samples]
    cases.append({"name": "xdbf-image", "source": str(sample),
                  "tree_path": image_path, "expected_size": expected_size,
                  "expected_sha256": expected_hash, "min_images": 1})
    manifest = output_dir / "manifest.json"
    manifest.write_text(json.dumps(cases), encoding="utf-8")
    result = subprocess.run([
        sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
        "--exe", str(peare_gui_path), "--output-dir", str(output_dir),
        "--manifest", str(manifest), "--report", str(output_dir / "report.json")],
        capture_output=True, text=True, timeout=120, check=False,
        env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "20 total, 20 passed, 0 failed." in result.stdout, result.stdout
