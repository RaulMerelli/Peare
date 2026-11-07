"""Check authentic XBCOMPRESS samples through the public Opener ABI."""

import ctypes as C
import hashlib
import os
from pathlib import Path

import pytest
import peare_opener as po


EXPECTED_FILE = os.environ.get("PEARE_XBCOMPRESS_INPUT")
LZMS_MULTIBLOCK_SAMPLE = os.environ.get("PEARE_XBCOMPRESS_LZMS_MULTIBLOCK_SAMPLE")
LZMS_MULTIBLOCK_INPUT = os.environ.get("PEARE_XBCOMPRESS_LZMS_MULTIBLOCK_INPUT")


def _assert_payload_matches(peare_lib, sample, expected):
    opener = peare_lib.open_file(sample)
    assert opener is not None, (sample, peare_lib.status_message(peare_lib.last_open_status))
    with opener:
        assert opener.container_format() == "XBCOMPRESS"
        assert opener.folder_count() == 1
        assert opener.resource_count(0) == 1

        resource = po._HANDLE()
        assert peare_lib._lib.peare_opener_open_resource_at(
            opener._h, 0, 0, C.byref(resource)
        ) == 0
        try:
            context = po._Context()
            assert peare_lib._lib.peare_resource_get_context(
                resource, C.byref(context)
            ) == 0
            try:
                assert context.data_size == len(expected)
            finally:
                peare_lib._lib.peare_resource_context_free(C.byref(context))

            blob = po._Blob()
            assert peare_lib._lib.peare_resource_get_payload(
                resource, C.byref(blob)
            ) == 0
            try:
                actual = blob.to_bytes()
            finally:
                peare_lib._lib.peare_blob_free(C.byref(blob))
        finally:
            peare_lib._lib.peare_resource_destroy(resource)
    assert hashlib.sha256(actual).digest() == hashlib.sha256(expected).digest()


@pytest.mark.parametrize("sample_env", [
    "PEARE_XBCOMPRESS_LZMS_SAMPLE",
    "PEARE_XBCOMPRESS_MSZIP_SAMPLE",
])
def test_xbcompress_algorithms_match_independent_input(peare_lib, sample_env):
    sample = os.environ.get(sample_env)
    if not sample or not EXPECTED_FILE:
        pytest.skip("Set the XBCOMPRESS sample and input environment variables")

    expected = Path(EXPECTED_FILE).read_bytes()
    _assert_payload_matches(peare_lib, sample, expected)


def test_xbcompress_lzms_multiple_blocks(peare_lib):
    if not LZMS_MULTIBLOCK_SAMPLE or not LZMS_MULTIBLOCK_INPUT:
        pytest.skip("Set the XBCOMPRESS LZMS multi-block sample variables")

    packed = Path(LZMS_MULTIBLOCK_SAMPLE).read_bytes()
    expected = Path(LZMS_MULTIBLOCK_INPUT).read_bytes()
    block_size = int.from_bytes(packed[32:40], "little")
    assert block_size > 0 and len(expected) > block_size
    _assert_payload_matches(peare_lib, LZMS_MULTIBLOCK_SAMPLE, expected)
