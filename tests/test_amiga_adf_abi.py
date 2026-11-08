import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

from conftest import ROOT


SAMPLE_DIR = Path(os.environ.get(
    "PEARE_AMIGA_ADF_DIR", ROOT / "tests" / "fixtures" / "amiga-adf"))
SAMPLES = {
    "testofs.adf": {
        "sha256": "b4a533173655f55785b3de2303b94893d79e970138a7755eb334d1a87746aa46",
        "format": "FLOPPY_IMAGE",
        "member": "MOON.GIF",
        "member_sha256": "2dc7bad47f0c94cf16d668fced31341ade09813db05e721aecbf7b2e7ad8bcd2",
        "member_size": 173847,
        "paths": {"MOON.GIF"},
    },
    "testffs.adf": {
        "sha256": "f0213c4460d98dd6fe0eef228887bb659cf0bac86bc6180d8c4e5dbc8da868f2",
        "format": "FLOPPY_IMAGE",
        "member": "dir_1/textfile.txt",
        "member_sha256": "2232bd0bed72101281be19f9cea75e2e03684200156eb629849b8b14c0a3ce86",
        "member_size": 12,
        "paths": {"dir_1", "dir_1/textfile.txt", "dir_2/blue2c.gif",
                  "same_hash/file_3a", "empty_dir", "emptyfile"},
    },
    "win32-names.adf": {
        "sha256": "059dbe30a2867b9f56693055681d6305e2365bcb1d3d3cd1b21792ee638d468d",
        "format": "FLOPPY_IMAGE",
        "member": "win32-names.sh",
        "member_sha256": "d1e26d8394edeef07e51e0337ba741c8778ec88f3ea9e9ec13756180105731ba",
        "member_size": 3398,
        "paths": {"forbidden-chars", "forbidden-files/CON",
                  "not-forbidden/AUXX", "win32-names.sh"},
    },
    "bootdisk-amiga-m68k.adf": {
        "sha256": "6001ada464bb1dfd9dd8309c6b897ab2b8c429e498e9141cd478cc808cd9f2e5",
        "format": "FLOPPY_IMAGE",
        "member": "README.txt",
        "member_sha256": "7ecb962a70de267f6aba2c74e5c9a2b491943e1ccad2763a750d8c32a7e940cf",
        "member_size": 204,
        "paths": {"C", "C/Copy", "Devs/DOSDrivers/PIPE", "README.txt",
                  "S/Startup-Sequence", "boot/amiga/AROSBootstrap"},
    },
}
ADZ_SAMPLE = "AROS-bootdisk-amiga-m68k.adz"
ADZ_SHA256 = "23aa739c49801a92329acdbcf0ffa54ce0b6a84698ec3709a808bbd1361f41cb"


def _sample(name):
    path = SAMPLE_DIR / name
    if not path.is_file():
        pytest.skip(f"Set PEARE_AMIGA_ADF_DIR to the external Amiga ADF samples")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == SAMPLES[name]["sha256"]
    return path


@pytest.mark.parametrize("sample_name", SAMPLES)
def test_amiga_adf_abi_reads_real_filesystems(peare_lib, sample_name):
    sample = _sample(sample_name)
    expected = SAMPLES[sample_name]
    with peare_lib.open_file(str(sample)) as opener:
        assert opener.container_format() == expected["format"]
        rows = opener.normalized_tree(max_depth=16)
    actual = {row["path"]: row for row in rows}
    assert expected["paths"] <= actual.keys()
    assert len(actual) == len(rows)
    member = actual[expected["member"]]
    assert member["size"] == expected["member_size"]
    assert member["sha256"] == expected["member_sha256"]


@pytest.mark.parametrize("sample_name", SAMPLES)
def test_amiga_adf_gui_previews_real_files(peare_gui_path, sample_name):
    sample = _sample(sample_name)
    expected = SAMPLES[sample_name]
    output = ROOT / "tests" / "output" / "amiga-adf" / sample_name.replace(".", "-")
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / "manifest.json"
    manifest.write_text(json.dumps([{
        "name": sample_name.replace(".", "-"),
        "source": str(sample),
        "tree_path": expected["member"],
        "expected_size": expected["member_size"],
        "expected_sha256": expected["member_sha256"],
        "expect_no_images": sample_name != "testofs.adf",
        "min_images": 1 if sample_name == "testofs.adf" else 0,
    }]), encoding="utf-8")
    result = subprocess.run([
        sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
        "--exe", str(peare_gui_path), "--output-dir", str(output),
        "--manifest", str(manifest), "--report", str(output / "report.json"),
    ], capture_output=True, text=True, timeout=120, check=False, env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 total, 1 passed, 0 failed." in result.stdout, result.stdout


def test_amiga_adz_abi_opens_compressed_boot_disk(peare_lib):
    sample = SAMPLE_DIR / ADZ_SAMPLE
    if not sample.is_file():
        pytest.skip("Set PEARE_AMIGA_ADF_DIR to the external Amiga ADZ sample")
    assert hashlib.sha256(sample.read_bytes()).hexdigest() == ADZ_SHA256
    with peare_lib.open_file(str(sample)) as opener:
        assert opener.container_format() == "FLOPPY_IMAGE"
        rows = opener.normalized_tree(max_depth=16)
    actual = {row["path"]: row for row in rows}
    assert {"C/Copy", "Devs/DOSDrivers/PIPE", "README.txt",
            "S/Startup-Sequence", "boot/amiga/AROSBootstrap"} <= actual.keys()
    assert actual["README.txt"]["size"] == 204
    assert actual["README.txt"]["sha256"] == SAMPLES[
        "bootdisk-amiga-m68k.adf"]["member_sha256"]


def test_amiga_adz_gui_previews_decompressed_file(peare_gui_path):
    sample = SAMPLE_DIR / ADZ_SAMPLE
    if not sample.is_file():
        pytest.skip("Set PEARE_AMIGA_ADF_DIR to the external Amiga ADZ sample")
    output = ROOT / "tests" / "output" / "amiga-adz"
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / "manifest.json"
    manifest.write_text(json.dumps([{
        "name": "aros-adz-readme", "source": str(sample),
        "tree_path": "README.txt", "expected_size": 204,
        "expected_sha256": SAMPLES["bootdisk-amiga-m68k.adf"]["member_sha256"],
        "expect_no_images": True,
    }]), encoding="utf-8")
    result = subprocess.run([
        sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
        "--exe", str(peare_gui_path), "--output-dir", str(output),
        "--manifest", str(manifest), "--report", str(output / "report.json"),
    ], capture_output=True, text=True, timeout=120, check=False, env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 total, 1 passed, 0 failed." in result.stdout, result.stdout
