"""Verify the external large-dictionary RAR7 sample through the Opener ABI."""

import os
from pathlib import Path
import zlib
import ctypes as C

import pytest

import peare_opener as po


SAMPLE_SIZE = 4_563_402_752
SAMPLE_CRC32 = 0x8E9B8BF4


def _sample_path():
    archive_value = os.environ.get("PEARE_RAR7_SAMPLE")
    if not archive_value:
        pytest.skip("Set PEARE_RAR7_SAMPLE to the external RAR7 oracle sample")
    archive = Path(archive_value)
    if not archive.is_file():
        pytest.skip(f"Missing external RAR7 sample: {archive}")
    return archive


def test_rar7_algorithm1_large_dictionary_sample_opens(peare_lib):
    archive = _sample_path()
    opener = peare_lib.open_file(str(archive))
    assert opener is not None, peare_lib.status_message(peare_lib.last_open_status)
    with opener:
        assert opener.container_format() == "RAR5"
        sizes = []
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
                        sizes.append(int(context.data_size))
                    finally:
                        peare_lib._lib.peare_resource_context_free(C.byref(context))
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)
        assert SAMPLE_SIZE in sizes


def test_rar7_algorithm1_large_dictionary_matches_oracle(peare_lib):
    if os.environ.get("PEARE_RAR7_VERIFY_FULL") != "1":
        pytest.skip("Set PEARE_RAR7_VERIFY_FULL=1 for the 4.56 GB decode oracle")
    archive = _sample_path()
    opener = peare_lib.open_file(str(archive))
    assert opener is not None, peare_lib.status_message(peare_lib.last_open_status)
    with opener:
        assert opener.container_format() == "RAR5"
        total = 0
        checksum = 0
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
                        size = int(context.data_size)
                    finally:
                        peare_lib._lib.peare_resource_context_free(C.byref(context))
                    offset = 0
                    while offset < size:
                        chunk = peare_lib.read_resource(
                            resource, offset, min(1024 * 1024, size - offset)
                        )
                        assert chunk
                        checksum = zlib.crc32(chunk, checksum)
                        total += len(chunk)
                        offset += len(chunk)
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)
        assert total == SAMPLE_SIZE
        assert checksum == SAMPLE_CRC32
