import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

import pytest

from conftest import ROOT


SAMPLE = Path(os.environ.get(
    "PEARE_WAD2_SAMPLE", ROOT / "tests" / "fixtures" / "wad" / "gfx.wad"))
SAMPLE_WAD3 = Path(os.environ.get(
    "PEARE_WAD3_SAMPLE", ROOT / "tests" / "fixtures" / "wad" / "decals.wad"))


def _records(path, signature):
    data = path.read_bytes()
    assert data[:4] == signature
    count, directory = struct.unpack_from("<II", data, 4)
    assert directory + count * 32 <= len(data)
    records = {}
    for index in range(count):
        entry = directory + index * 32
        offset, stored_size, size = struct.unpack_from("<III", data, entry)
        kind, compression = struct.unpack_from("<BB", data, entry + 12)
        name = data[entry + 16:entry + 32].split(b"\0", 1)[0].decode("ascii")
        assert compression in (0, 1)
        assert offset + stored_size <= len(data)
        payload = data[offset:offset + stored_size]
        records[name] = (stored_size, hashlib.sha256(payload).hexdigest(), kind, compression)
        if compression == 0:
            assert size == stored_size
    return records


def test_wad2_abi_lists_directory_and_reads_lumps(peare_lib):
    if not SAMPLE.is_file():
        pytest.skip("Set PEARE_WAD2_SAMPLE to the external Quake WAD2 sample")
    expected = _records(SAMPLE, b"WAD2")
    assert len(expected) == 163
    with peare_lib.open_file(str(SAMPLE)) as opener:
        assert opener.container_format() == "WAD"
        actual = {row["path"]: (row["size"], row["sha256"])
                  for row in opener.normalized_tree(recurse_containers=False)}
        assert set(actual) == set(expected)
        for name, (size, digest, _kind, compression) in expected.items():
            assert actual[name] == (size, digest), name
            assert compression == 0, name


def test_wad2_gui_opens_real_texture_lump(peare_gui_path):
    if not SAMPLE.is_file():
        pytest.skip("Set PEARE_WAD2_SAMPLE to the external Quake WAD2 sample")
    records = _records(SAMPLE, b"WAD2")
    size, digest, _kind, _compression = records["NUM_0"]
    output_dir = ROOT / "tests" / "output" / "wad2-gui"
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest = output_dir / "manifest.json"
    manifest.write_text(json.dumps([{
        "name": "quake-wad2-num-0", "source": str(SAMPLE),
        "tree_path": "NUM_0", "expected_size": size,
        "expected_sha256": digest, "expect_no_images": True,
    }]), encoding="utf-8")
    result = subprocess.run([
        sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
        "--exe", str(peare_gui_path), "--output-dir", str(output_dir),
        "--manifest", str(manifest), "--report", str(output_dir / "report.json")],
        capture_output=True, text=True, timeout=120, check=False,
        env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 total, 1 passed, 0 failed." in result.stdout, result.stdout


def test_wad3_abi_lists_directory_and_reads_lumps(peare_lib):
    if not SAMPLE_WAD3.is_file():
        pytest.skip("Set PEARE_WAD3_SAMPLE to the external GoldSrc WAD3 sample")
    expected = _records(SAMPLE_WAD3, b"WAD3")
    assert len(expected) == 222
    with peare_lib.open_file(str(SAMPLE_WAD3)) as opener:
        assert opener.container_format() == "WAD"
        actual = {row["path"]: (row["size"], row["sha256"])
                  for row in opener.normalized_tree(recurse_containers=False)}
        assert set(actual) == set(expected)
        for name, (size, digest, _kind, compression) in expected.items():
            assert actual[name] == (size, digest), name
            assert compression == 0, name


def test_wad3_gui_opens_real_texture_lump(peare_gui_path):
    if not SAMPLE_WAD3.is_file():
        pytest.skip("Set PEARE_WAD3_SAMPLE to the external GoldSrc WAD3 sample")
    records = _records(SAMPLE_WAD3, b"WAD3")
    size, digest, _kind, _compression = records["REFLECT1"]
    output_dir = ROOT / "tests" / "output" / "wad3-gui"
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest = output_dir / "manifest.json"
    manifest.write_text(json.dumps([{
        "name": "goldsrc-wad3-reflect1", "source": str(SAMPLE_WAD3),
        "tree_path": "REFLECT1", "expected_size": size,
        "expected_sha256": digest, "expect_no_images": True,
    }]), encoding="utf-8")
    result = subprocess.run([
        sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
        "--exe", str(peare_gui_path), "--output-dir", str(output_dir),
        "--manifest", str(manifest), "--report", str(output_dir / "report.json")],
        capture_output=True, text=True, timeout=120, check=False,
        env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 total, 1 passed, 0 failed." in result.stdout, result.stdout
