"""Check upstream UUencode samples through the public Opener ABI."""

import ctypes as C
import os
from pathlib import Path

import pytest
import peare_opener as po


DEFAULT_ROOT = Path(
    Path.home() / "Documents" / "inerente a" / "Peare" / "compress-samples" / "libarchive"
)
SAMPLES = (
    ("test_compat_mac-1.tar.Z.uu", "test_compat_mac-1.tar.Z"),
    ("test_compat_mac-2.tar.Z.uu", "test_compat_mac-2.tar.Z"),
)


def _payload(peare_lib, opener):
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
            assert context.type_utf8.to_bytes() == b"UUE_FILE"
            name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
        finally:
            peare_lib._lib.peare_resource_context_free(C.byref(context))
        blob = po._Blob()
        assert peare_lib._lib.peare_resource_get_payload(resource, C.byref(blob)) == 0
        try:
            return name, blob.to_bytes()
        finally:
            peare_lib._lib.peare_blob_free(C.byref(blob))
    finally:
        peare_lib._lib.peare_resource_destroy(resource)


@pytest.mark.parametrize(("encoded_name", "decoded_name"), SAMPLES)
def test_uue_payload_matches_upstream_pair(peare_lib, encoded_name, decoded_name):
    root = Path(os.environ.get("PEARE_COMPRESS_SAMPLE_ROOT", DEFAULT_ROOT))
    encoded = root / encoded_name
    expected = root / decoded_name
    if not encoded.is_file() or not expected.is_file():
        pytest.skip(f"Missing external libarchive sample pair: {encoded}")

    opener = peare_lib.open_file(str(encoded))
    assert opener is not None
    with opener:
        assert opener.container_format() == "UUE"
        name, payload = _payload(peare_lib, opener)
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
            assert child.container_format() == "UNIX_COMPRESS"
    assert name == decoded_name
    assert payload == expected.read_bytes()


def test_rfc_begin_base64_block_with_preamble_and_trailer(peare_lib):
    encoded = os.environ.get("PEARE_UUE_RFC_SAMPLE")
    expected = os.environ.get("PEARE_UUE_RFC_PAYLOAD")
    if not encoded or not expected or not Path(encoded).is_file() or not Path(expected).is_file():
        pytest.skip("Missing external RFC 6873 UUencode sample pair")

    opener = peare_lib.open_file(encoded)
    assert opener is not None
    with opener:
        assert opener.container_format() == "UUE"
        name, payload = _payload(peare_lib, opener)
    assert name == "clf_record"
    assert payload == Path(expected).read_bytes()
