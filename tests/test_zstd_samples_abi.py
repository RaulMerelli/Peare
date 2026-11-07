"""Check independent Zstandard samples through the public Opener ABI."""

import hashlib
import json
import os
from pathlib import Path
import subprocess

import ctypes as C
import pytest
import peare_opener as po


REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = REPO / "tests" / "formats" / "zstd" / "fixtures"


def _payload(peare_lib, opener):
    assert opener.folder_count() == 1
    assert opener.resource_count(0) == 1
    resource = po._HANDLE()
    assert peare_lib._lib.peare_opener_open_resource_at(
        opener._h, 0, 0, C.byref(resource)
    ) == 0
    try:
        blob = po._Blob()
        assert peare_lib._lib.peare_resource_get_payload(resource, C.byref(blob)) == 0
        try:
            return blob.to_bytes()
        finally:
            peare_lib._lib.peare_blob_free(C.byref(blob))
    finally:
        peare_lib._lib.peare_resource_destroy(resource)


def test_zstd_payload_matches_independent_encoder(peare_lib):
    sample = DEFAULT_ROOT / "readme.zst"
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZSTD"
        _assert_expected_payload("readme", _payload(peare_lib, opener))


def test_zstd_frame_without_checksum(peare_lib):
    sample = DEFAULT_ROOT / "readme-no-check.zst"
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZSTD"
        _assert_expected_payload("readme-no-check", _payload(peare_lib, opener))


def test_zstd_empty_frame(peare_lib):
    opener = peare_lib.open_file(str(DEFAULT_ROOT / "empty.zst"))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZSTD"
        assert _payload(peare_lib, opener) == b""


def test_zstd_concatenated_frames(peare_lib):
    opener = peare_lib.open_file(str(DEFAULT_ROOT / "multi-frame.zst"))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZSTD"
        _assert_expected_payload("multi-frame", _payload(peare_lib, opener))


def test_zstd_payload_opens_nested_tar_through_abi(peare_lib):
    sample = DEFAULT_ROOT / "docs.tar.zst"
    expected = REPO / "tests" / "formats" / "tar" / "fixtures" / "docs.tar"
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ZSTD"
        assert _payload(peare_lib, opener) == expected.read_bytes()
        resource = po._HANDLE()
        assert peare_lib._lib.peare_opener_open_resource_at(
            opener._h, 0, 0, C.byref(resource)
        ) == 0
        try:
            source = po._HANDLE()
            assert peare_lib._lib.peare_resource_get_source(resource, C.byref(source)) == 0
        finally:
            peare_lib._lib.peare_resource_destroy(resource)
    child = peare_lib._open_source(source)
    peare_lib._lib.peare_source_destroy(source)
    assert child is not None
    with child:
        assert child.container_format() == "TAR"


def test_zstd_upstream_golden_frames(peare_lib):
    root_value = os.environ.get("PEARE_ZSTD_UPSTREAM_SAMPLE_ROOT")
    cli_value = os.environ.get("PEARE_ZSTD_CLI")
    if not root_value or not cli_value:
        pytest.skip("Set upstream Zstandard sample root and CLI paths")
    samples = sorted(Path(root_value).glob("*.zst"))
    assert samples
    for sample in samples:
        oracle = subprocess.run(
            [cli_value, "-d", "-c", str(sample)], capture_output=True, check=True
        )
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample
        with opener:
            assert opener.container_format() == "ZSTD", sample
            assert _payload(peare_lib, opener) == oracle.stdout, sample


def test_zstd_rejects_upstream_truncated_frame(peare_lib):
    root_value = os.environ.get("PEARE_ZSTD_ERROR_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_ZSTD_ERROR_SAMPLE_ROOT to the upstream error corpus")
    sample = Path(root_value) / "truncated_huff_state.zst"
    if not sample.is_file():
        pytest.skip(f"Missing upstream error sample: {sample}")
    opener = peare_lib.open_file(str(sample))
    if opener is None:
        return
    with opener:
        assert opener.container_format() == "ZSTD"
        assert opener.folder_count() == 0


def _assert_expected_payload(name, payload):
    manifest_path = REPO / "tests" / "formats" / "zstd" / "expected" / f"{name}.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))["tree"][0]
    assert len(payload) == manifest["size"]
    assert hashlib.sha256(payload).hexdigest() == manifest["sha256"]
