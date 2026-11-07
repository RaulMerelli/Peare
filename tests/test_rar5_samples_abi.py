"""Check RAR5 members through the public Opener ABI."""

import os
from pathlib import Path
import subprocess
import zlib

import pytest

from test_rar4_samples_abi import _rar_payload, _seven_zip


@pytest.mark.parametrize("sample_name", (
    "rar5-stored.rar",
    "rar5-m1.rar",
    "rar5-m2.rar",
    "rar5-m3.rar",
    "rar5-m4.rar",
    "rar5-m5.rar",
))
def test_rar5_members_match_7zip(peare_lib, sample_name):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "winrar" / sample_name
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    oracle = subprocess.run(
        [seven_zip, "x", "-so", str(archive), "CMakeLists.txt"],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, "CMakeLists.txt", None, "RAR5")
    assert actual == oracle


@pytest.mark.parametrize("entry_name", ("README.md", "AGENTS.md"))
def test_rar5_solid_members_match_7zip(peare_lib, entry_name):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "winrar" / "rar5-solid-m5.rar"
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    oracle = subprocess.run(
        [seven_zip, "x", "-so", str(archive), entry_name],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, entry_name, None, "RAR5")
    assert actual == oracle


def test_rar5_standard_filters_match_7zip(peare_lib):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "winrar" / "rar5-filter.exe.rar"
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    oracle = subprocess.run(
        [seven_zip, "x", "-so", str(archive), "Peare.exe"],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, "Peare.exe", None, "RAR5")
    assert actual == oracle


def test_rar5_arm32_elf_member_matches_7zip(peare_lib):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "winrar" / "rar5-arm32-oemcamera.rar"
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    oracle = subprocess.run(
        [seven_zip, "x", "-so", str(archive), "liboemcamera.so"],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, "liboemcamera.so", None, "RAR5")
    assert actual == oracle


def test_rar5_arm_filter_matches_libarchive_sample(peare_lib):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "libarchive" / "rar5-arm-filter.rar"
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 ARM filter sample: {archive.name}")
    entry_name = "elf-Linux-ARMv7-ls"
    oracle = subprocess.run(
        [seven_zip, "x", "-so", str(archive), entry_name],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, entry_name, None, "RAR5")
    assert len(oracle) == 90808
    assert zlib.crc32(oracle) == 0x886F91EB
    assert actual == oracle


@pytest.mark.parametrize("sample_name", (
    "rar5-split.part1.rar",
    "rar5-split-m5.part1.rar",
    "rar5-split-encrypted.part1.rar",
    "rar5-split-encrypted-headers.part1.rar",
))
def test_rar5_split_volume_member_matches_7zip(peare_lib, sample_name):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "winrar" / sample_name
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    password = "PeareRAR5" if "encrypted" in sample_name else None
    command = [seven_zip, "x", "-so"]
    if password:
        command.append(f"-p{password}")
    oracle = subprocess.run(
        command + [str(archive), "Peare.exe"],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, "Peare.exe", password, "RAR5")
    assert actual == oracle


@pytest.mark.parametrize("sample_name", (
    "rar5-encrypted-store.rar",
    "rar5-encrypted-m5.rar",
))
def test_rar5_encrypted_members_match_7zip(peare_lib, sample_name):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "winrar" / sample_name
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    oracle = subprocess.run(
        [seven_zip, "x", "-so", "-pPeareRAR5", str(archive), "README.md"],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, "README.md", "PeareRAR5", "RAR5")
    assert actual == oracle


def test_rar5_encrypted_member_rejects_wrong_password(peare_lib):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    archive = Path(root_value) / "winrar" / "rar5-encrypted-store.rar"
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    with pytest.raises(RuntimeError, match="Resource integrity verification failed"):
        _rar_payload(peare_lib, archive, "README.md", "incorrect", "RAR5")


@pytest.mark.parametrize("entry_name", ("README.md", "AGENTS.md"))
def test_rar5_encrypted_headers_match_7zip(peare_lib, entry_name):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "winrar" / "rar5-encrypted-headers.rar"
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    oracle = subprocess.run(
        [seven_zip, "x", "-so", "-pPeareRAR5", str(archive), entry_name],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, entry_name, "PeareRAR5", "RAR5")
    assert actual == oracle


def test_rar5_encrypted_headers_require_valid_password(peare_lib):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    archive = Path(root_value) / "winrar" / "rar5-encrypted-headers.rar"
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    assert peare_lib.open_file(str(archive)) is None
    assert peare_lib.open_file(str(archive), password="incorrect") is None


def test_rar5_recovery_record_archive_matches_7zip(peare_lib):
    sample_value = os.environ.get("PEARE_RAR5_RECOVERY_SAMPLE")
    expected_value = os.environ.get("PEARE_RAR5_RECOVERY_EXPECTED")
    if not sample_value or not expected_value:
        pytest.skip("Set RAR5 recovery sample and expected payload paths")
    archive = Path(sample_value)
    expected = Path(expected_value).read_bytes()
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    oracle = subprocess.run(
        [seven_zip, "x", "-so", str(archive), "README.md"],
        check=True,
        capture_output=True,
    ).stdout
    assert oracle == expected
    actual = _rar_payload(peare_lib, archive, "README.md", None, "RAR5")
    assert actual == oracle
