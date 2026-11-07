"""Check GNU Lzip samples through the public Opener ABI."""

import os
from pathlib import Path

import ctypes as C
import pytest
import peare_opener as po


REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = REPO / "tests" / "formats" / "lzip" / "fixtures"
INVALID_SAMPLES = (
    "fox_v2.lz",
    "fox_s11.lz",
    "fox_de20.lz",
    "fox_bcrc.lz",
    "fox_crc0.lz",
    "fox_das46.lz",
    "fox_mes81.lz",
)


def _sample_root():
    return Path(os.environ.get("PEARE_LZIP_SAMPLE_ROOT", DEFAULT_ROOT))


def _payload(peare_lib, opener):
    assert opener.folder_count() == 1
    assert opener.resource_count(0) == 1
    resource = po._HANDLE()
    assert peare_lib._lib.peare_opener_open_resource_at(
        opener._h, 0, 0, C.byref(resource)
    ) == 0
    try:
        blob = po._Blob()
        assert peare_lib._lib.peare_resource_get_payload(
            resource, C.byref(blob)
        ) == 0
        try:
            return blob.to_bytes()
        finally:
            peare_lib._lib.peare_blob_free(C.byref(blob))
    finally:
        peare_lib._lib.peare_resource_destroy(resource)


def test_lzip_payload_matches_upstream_source(peare_lib):
    root = _sample_root()
    sample = root / "test.txt.lz"
    expected = root / "test.txt"
    if not sample.is_file() or not expected.is_file():
        pytest.skip(f"Missing GNU Lzip sample pair: {sample}")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "LZIP"
        assert _payload(peare_lib, opener) == expected.read_bytes()


def test_lzip_concatenated_members_from_independent_encoder(peare_lib):
    sample_value = os.environ.get("PEARE_LZIP_MULTI_MEMBER_SAMPLE")
    if not sample_value:
        pytest.skip("Missing multi-member Lzip sample path")
    sample = Path(sample_value)
    source = _sample_root() / "test.txt"
    if not sample.is_file() or not source.is_file():
        pytest.skip(f"Missing multi-member Lzip sample or source: {sample}")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "LZIP"
        assert _payload(peare_lib, opener) == source.read_bytes() + (REPO / "README.md").read_bytes()


def test_lzip_empty_single_member_from_independent_encoder(peare_lib):
    sample_value = os.environ.get("PEARE_LZIP_EMPTY_SAMPLE")
    if not sample_value:
        pytest.skip("Missing empty Lzip sample path")
    sample = Path(sample_value)
    if not sample.is_file():
        pytest.skip(f"Missing empty Lzip sample: {sample}")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "LZIP"
        assert _payload(peare_lib, opener) == b""


def test_lzip_payload_opens_nested_tar_through_abi(peare_lib):
    sample_value = os.environ.get("PEARE_LZIP_TAR_SAMPLE")
    if not sample_value:
        pytest.skip("Missing Lzip-compressed TAR sample path")
    sample = Path(sample_value)
    expected = REPO / "tests" / "formats" / "tar" / "fixtures" / "docs.tar"
    if not sample.is_file() or not expected.is_file():
        pytest.skip(f"Missing Lzip TAR sample or source: {sample}")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "LZIP"
        assert _payload(peare_lib, opener) == expected.read_bytes()
        resource = po._HANDLE()
        assert peare_lib._lib.peare_opener_open_resource_at(
            opener._h, 0, 0, C.byref(resource)
        ) == 0
        try:
            source = po._HANDLE()
            assert peare_lib._lib.peare_resource_get_source(
                resource, C.byref(source)
            ) == 0
        finally:
            peare_lib._lib.peare_resource_destroy(resource)
    child = peare_lib._open_source(source)
    peare_lib._lib.peare_source_destroy(source)
    assert child is not None
    with child:
        assert child.container_format() == "TAR"


@pytest.mark.parametrize("sample_name", INVALID_SAMPLES)
def test_lzip_rejects_upstream_corruption_samples(peare_lib, sample_name):
    sample = _sample_root() / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing GNU Lzip sample: {sample}")

    opener = peare_lib.open_file(str(sample))
    if opener is None:
        return
    with opener:
        assert opener.container_format() == "LZIP"
        assert opener.folder_count() == 0
