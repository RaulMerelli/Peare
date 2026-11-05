"""Check a real LZX CAB sample through the public Opener ABI."""

import ctypes as C
import hashlib
import os
from pathlib import Path

import pytest
import peare_opener as po


DEFAULT_SAMPLE = (
    Path.home()
    / "Documents"
    / "inerente a"
    / "cerf"
    / "[plc247.com]SiemensProSave_V18"
    / "Siemens_Simatic_Prosave_V18"
    / "Data1.cab"
)
EXPECTED_SIZE = 17_812_242
EXPECTED_SHA256 = "c4bef7acbeb264e218e95a609f5eb0b0a9feadb40d6fe978c5676a14e189abf5"


def test_real_lzx15_cab_members_match_7zip_output(peare_lib):
    sample = Path(os.environ.get("PEARE_CAB_LZX15_SAMPLE", DEFAULT_SAMPLE))
    if not sample.is_file():
        pytest.skip(f"Missing external LZX15 CAB sample: {sample}")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    digest = hashlib.sha256()
    total_size = 0
    member_count = 0
    with opener:
        assert opener.container_format() == "CAB"
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = po._HANDLE()
                assert peare_lib._lib.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)
                ) == 0
                try:
                    blob = po._Blob()
                    assert peare_lib._lib.peare_resource_get_payload(
                        resource, C.byref(blob)
                    ) == 0
                    try:
                        payload = blob.to_bytes()
                    finally:
                        peare_lib._lib.peare_blob_free(C.byref(blob))
                    digest.update(payload)
                    total_size += len(payload)
                    member_count += 1
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)

    assert member_count == 56
    assert total_size == EXPECTED_SIZE
    assert digest.hexdigest() == EXPECTED_SHA256
