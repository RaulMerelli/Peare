"""Check real SZDD mode B files through the public Opener ABI."""

import os
import ctypes as C
from pathlib import Path

import pytest
import peare_opener as po


SAMPLE_ROOT = os.environ.get("PEARE_SZDD_B_SAMPLE_ROOT")


def _samples():
    if not SAMPLE_ROOT:
        return []
    root = Path(SAMPLE_ROOT)
    return sorted(
        path for path in root.rglob("*")
        if path.is_file()
        and (packed := path.read_bytes()).startswith(b"SZDD\x88\xf0\x27\x33")
        and len(packed) >= 14
        and packed[8] == ord("B")
    )


@pytest.mark.parametrize("sample", _samples(), ids=lambda path: path.name)
def test_szdd_mode_b_real_sample_matches_declared_size(peare_lib, sample):
    packed = sample.read_bytes()
    expected_size = int.from_bytes(packed[10:14], "little")
    assert expected_size <= 16 * 1024 * 1024

    opener = peare_lib.open_file(str(sample))
    assert opener is not None, f"Peare failed to open {sample}"
    with opener:
        assert opener.container_format() == "SZDD"
        leaves = []
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                handle = po._HANDLE()
                assert peare_lib._lib.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(handle)
                ) == 0
                try:
                    context = po._Context()
                    assert peare_lib._lib.peare_resource_get_context(
                        handle, C.byref(context)
                    ) == 0
                    try:
                        if context.type_utf8.to_bytes() == b"SZDD_FILE":
                            prefix = peare_lib.read_resource(handle, 0, 4)
                            leaves.append((context.data_size, prefix))
                    finally:
                        peare_lib._lib.peare_resource_context_free(C.byref(context))
                finally:
                    if handle:
                        peare_lib._lib.peare_resource_destroy(handle)
        assert len(leaves) == 1
        size, prefix = leaves[0]
        assert size == expected_size
        expected_prefixes = {
            "COMM.DR_": b"MZ",
            "KERNEL.EX_": b"MZ",
            "GDI.EX_": b"MZ",
            "USER.EX_": b"MZ",
            "SETUP.HL_": b"\x3f\x5f",
        }
        expected_prefix = expected_prefixes.get(sample.name.upper())
        if expected_prefix:
            assert prefix.startswith(expected_prefix)
