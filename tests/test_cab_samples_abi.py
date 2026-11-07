"""Check CAB compression variants through the public Opener ABI."""

import ctypes as C
import os
from pathlib import Path
import subprocess

import pytest
import peare_opener as po

from test_rar4_samples_abi import _seven_zip

FIXTURES = Path(__file__).resolve().parent / "formats" / "cab" / "fixtures"


def _cab_members(peare_lib, sample):
    opener = peare_lib.open_file(str(sample))
    assert opener is not None, peare_lib.status_message(peare_lib.last_open_status)
    members = []
    with opener:
        assert opener.container_format() == "CAB"
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = po._HANDLE()
                assert peare_lib._lib.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)
                ) == 0
                try:
                    context = po._Context()
                    assert peare_lib._lib.peare_resource_get_context(
                        resource, C.byref(context)
                    ) == 0
                    try:
                        name = context.identifier_utf8.to_bytes().decode(
                            "utf-8", "replace"
                        )
                        size = int(context.data_size)
                    finally:
                        peare_lib._lib.peare_resource_context_free(C.byref(context))
                    payload = bytearray()
                    offset = 0
                    while offset < size:
                        chunk = peare_lib.read_resource(
                            resource, offset, min(64 * 1024, size - offset)
                        )
                        assert chunk
                        payload.extend(chunk)
                        offset += len(chunk)
                    members.append((name, bytes(payload)))
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)
    return members


def _assert_matches_7zip(peare_lib, sample):
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    members = _cab_members(peare_lib, sample)
    assert members
    for name, actual in members:
        oracle = subprocess.run(
            [seven_zip, "x", "-so", str(sample), name],
            check=True,
            capture_output=True,
        ).stdout
        assert actual == oracle, name


@pytest.mark.parametrize(("variable", "filename"), (
    ("PEARE_CAB_UNCOMPRESSED_SAMPLE", "uncompressed-wincearm.cab"),
    ("PEARE_CAB_MSZIP_SAMPLE", "mszip-adk.cab"),
    ("PEARE_CAB_LZX15_SAMPLE", "lzx15-prosave.cab"),
))
def test_real_cab_variants_match_7zip(peare_lib, variable, filename):
    root = os.environ.get("PEARE_CAB_SAMPLE_ROOT")
    sample_value = os.environ.get(variable)
    sample = Path(sample_value) if sample_value else (
        Path(root) / filename if root else Path()
    )
    if not sample.is_file():
        pytest.skip(f"Set PEARE_CAB_SAMPLE_ROOT or {variable} to a real CAB sample")
    _assert_matches_7zip(peare_lib, sample)


def test_makecab_lzx21_fixture_matches_7zip(peare_lib):
    _assert_matches_7zip(peare_lib, FIXTURES / "readme-lzx21.cab")


@pytest.mark.parametrize("window_bits", range(10, 22))
def test_quantum_windows_match_7zip(peare_lib, window_bits):
    root = os.environ.get("PEARE_CAB_QUANTUM_SAMPLE_ROOT")
    if not root:
        pytest.skip("Set PEARE_CAB_QUANTUM_SAMPLE_ROOT to Quantum CAB samples")
    sample = Path(root) / f"quantum-{window_bits}.cab"
    assert sample.is_file()
    _assert_matches_7zip(peare_lib, sample)


def test_real_mixed_quantum_cab_matches_7zip(peare_lib):
    root = os.environ.get("PEARE_CAB_QUANTUM_SAMPLE_ROOT")
    if not root:
        pytest.skip("Set PEARE_CAB_QUANTUM_SAMPLE_ROOT to Quantum CAB samples")
    _assert_matches_7zip(peare_lib, Path(root) / "mszip_lzx_qtm.cab")


def test_quantum_history_across_blocks_matches_7zip(peare_lib):
    root = os.environ.get("PEARE_CAB_QUANTUM_SAMPLE_ROOT")
    if not root:
        pytest.skip("Set PEARE_CAB_QUANTUM_SAMPLE_ROOT to Quantum CAB samples")
    _assert_matches_7zip(peare_lib, Path(root) / "quantum-high-history.cab")
