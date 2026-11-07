"""Check ZIPX compression methods through the public Opener ABI."""

from pathlib import Path, PurePosixPath
import hashlib
import os
import zipfile
import zlib

import ctypes as C
import pytest
import peare_opener as po


REPO = Path(__file__).resolve().parents[1]
FIXTURES = REPO / "tests" / "formats" / "zipx" / "fixtures"
METHODS = ("BZip2", "Deflate64", "LZMA", "PPMd", "PPMd-cutoff", "XZ", "Zstandard")
README_SAMPLE_SIZE = 2062
README_SAMPLE_SHA256 = "84f1b1405488f766cfeae58d72cd7d73aef6d38654bb16887f13ef337fd659a0"
DCL_VARIANTS = (
    "binary-1024",
    "binary-2048",
    "binary-4096",
    "ascii-1024",
    "ascii-2048",
    "ascii-4096",
)


def _assert_readme_sample(payload):
    assert len(payload) == README_SAMPLE_SIZE
    assert hashlib.sha256(payload).hexdigest() == README_SAMPLE_SHA256


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


def _payload_at(peare_lib, opener, folder, index):
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
            return blob.to_bytes()
        finally:
            peare_lib._lib.peare_blob_free(C.byref(blob))
    finally:
        peare_lib._lib.peare_resource_destroy(resource)


@pytest.mark.parametrize("method", METHODS)
def test_zipx_method_payload_matches_source(peare_lib, method):
    sample = FIXTURES / f"README-{method}.zipx"
    assert sample.is_file()

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        _assert_readme_sample(_payload(peare_lib, opener))


@pytest.mark.parametrize("variant", DCL_VARIANTS)
def test_zip_method_10_dcl_implode_matches_source(peare_lib, variant):
    sample = FIXTURES / f"README-DCL-{variant}.zip"
    assert sample.is_file()

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        _assert_readme_sample(_payload(peare_lib, opener))


def test_zipx_deflate64_large_source_matches_source(peare_lib):
    sample = FIXTURES / "ZipModule-Deflate64.zipx"
    assert sample.is_file()

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        payload = _payload(peare_lib, opener)
        assert len(payload) == 52792
        assert hashlib.sha256(payload).hexdigest() == (
            "710682de7e5883fad49bf884bf75d83b13dd954c061669a769a42c2309a00305"
        )


def test_external_zipx_ppmd_sample_matches_7zip_oracle(peare_lib):
    sample_value = os.environ.get("PEARE_ZIPX_EXTERNAL_SAMPLE")
    if not sample_value:
        pytest.skip("Set PEARE_ZIPX_EXTERNAL_SAMPLE to the external ZIPX sample")
    sample = Path(sample_value)
    assert sample.is_file()

    expected = {
        "sample.html": (83, "6d3ffb986ebb1d4d71d8fbbf98fb6edfba7772c83cff75dd871b77dc5ff1ceeb"),
        "Welcome.class": (431, "4ac4b584b74937675dc7d521690b442ef2545bef618c1e1bc0662d910388fb86"),
        "Welcome.java": (126, "7294342676155635ddcdfff15a7596517027fd2836bfba0ae40d70450c9cf3dc"),
        "sample.css": (302, "41ff902f13b99814df27913a9cde133896a0f30cc9b2df7912ea7476c3cb72ef"),
    }
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        rows = opener.normalized_tree()
        assert {row["path"]: (row["size"], row["sha256"]) for row in rows} == expected


def test_tool_created_zipx_ppmd_sample_matches_source(peare_lib):
    sample_value = os.environ.get("PEARE_ZIPX_PPMD_SAMPLE")
    if not sample_value:
        pytest.skip("Set PEARE_ZIPX_PPMD_SAMPLE to the local 7-Zip PPMd sample")
    sample = Path(sample_value)
    expected = REPO / "README.md"
    assert sample.is_file()

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        assert _payload(peare_lib, opener) == expected.read_bytes()


def test_winzip_zipx_ppmd_freeze_fixture_matches_source(peare_lib):
    sample = FIXTURES / "README-PPMd-freeze.zipx"
    assert sample.is_file()

    with zipfile.ZipFile(sample) as archive:
        members = [info for info in archive.infolist() if not info.is_dir()]
    assert len(members) == 1
    assert members[0].compress_type == 98
    assert (members[0].flag_bits & 0x0006) == 0

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        _assert_readme_sample(_payload(peare_lib, opener))


def test_external_zipx_wavpack_members_match_source_waves(peare_lib):
    sample_value = os.environ.get("PEARE_ZIPX_WAVPACK_SAMPLE")
    oracle_value = os.environ.get("PEARE_ZIPX_WAVPACK_ORACLE_DIR")
    if not sample_value or not oracle_value:
        pytest.skip("Set ZIPX WavPack sample and source WAV directory")
    sample = Path(sample_value)
    oracle_dir = Path(oracle_value)
    assert sample.is_file()
    assert oracle_dir.is_dir()

    with zipfile.ZipFile(sample) as archive:
        members = [info for info in archive.infolist() if not info.is_dir()]
    assert members
    assert all(info.compress_type == 97 for info in members)

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        assert opener.folder_count() == 1
        assert opener.resource_count(0) == len(members)
        for index, info in enumerate(members):
            assert Path(info.filename).name == info.filename
            source = oracle_dir / info.filename
            expected = source.read_bytes()
            payload = _payload_at(peare_lib, opener, 0, index)
            assert len(payload) == info.file_size
            assert zlib.crc32(payload) & 0xFFFFFFFF == info.CRC
            assert hashlib.sha256(payload).digest() == hashlib.sha256(expected).digest()


def test_external_zipx_jpeg_members_match_source_files(peare_lib):
    sample_value = os.environ.get("PEARE_ZIPX_JPEG_SAMPLE")
    source_value = os.environ.get("PEARE_ZIPX_JPEG_SOURCE_DIR")
    if not sample_value or not source_value:
        pytest.skip("Set ZIPX JPEG sample and source directory")
    sample = Path(sample_value)
    source_dir = Path(source_value)
    assert sample.is_file()
    assert source_dir.is_dir()

    with zipfile.ZipFile(sample) as archive:
        members = [info for info in archive.infolist() if not info.is_dir()]
    assert members
    assert all(info.compress_type == 96 for info in members)

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        assert opener.folder_count() == 1
        assert opener.resource_count(0) == len(members)
        for index, info in enumerate(members):
            assert "\\" not in info.filename
            relative = PurePosixPath(info.filename)
            assert not relative.is_absolute()
            assert ".." not in relative.parts
            source = source_dir.joinpath(*relative.parts)
            assert source.is_file()
            expected = source.read_bytes()
            payload = _payload_at(peare_lib, opener, 0, index)
            assert payload == expected
            assert len(payload) == info.file_size
            assert zlib.crc32(payload) & 0xFFFFFFFF == info.CRC


def test_external_zipx_mp3_member_matches_source(peare_lib):
    sample_value = os.environ.get("PEARE_ZIPX_MP3_SAMPLE")
    source_value = os.environ.get("PEARE_ZIPX_MP3_SOURCE")
    if not sample_value or not source_value:
        pytest.skip("Set ZIPX MP3 sample and source file")
    sample = Path(sample_value)
    source = Path(source_value)
    assert sample.is_file()
    assert source.is_file()

    with zipfile.ZipFile(sample) as archive:
        members = [info for info in archive.infolist() if not info.is_dir()]
    assert len(members) == 1
    info = members[0]
    assert info.compress_type == 94

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZIP"
        assert opener.folder_count() == 1
        assert opener.resource_count(0) == 1
        payload = _payload_at(peare_lib, opener, 0, 0)
        expected = source.read_bytes()
        assert payload == expected
        assert len(payload) == info.file_size
        assert zlib.crc32(payload) & 0xFFFFFFFF == info.CRC
