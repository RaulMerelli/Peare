"""Test independent XZ samples through the public Opener ABI."""

import hashlib
import json
import os
from pathlib import Path

import pytest
import peare_opener as po


REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = REPO / "tests" / "formats" / "xz" / "fixtures"


@pytest.mark.parametrize(
    "sample_name",
    [
        "docs.tar.xz",
        "docs-crc64.xz",
        "README-sha256.xz",
        "docs.tar.txz",
        "README.md.xz",
        "concatenated.xz",
        "no-extension.bin",
    ],
)
def test_independently_created_xz_samples_match_original_payload(peare_lib, sample_name):
    root = Path(os.environ.get("PEARE_XZ_SAMPLE_ROOT", DEFAULT_ROOT))
    sample = root / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing independent XZ sample: {sample}")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "XZ"
        assert opener.folder_count() == 1
        assert opener.resource_count(0) == 1
        resource = po._HANDLE()
        assert peare_lib._lib.peare_opener_open_resource_at(
            opener._h, 0, 0, po.C.byref(resource)
        ) == 0
        try:
            blob = po._Blob()
            assert peare_lib._lib.peare_resource_get_payload(
                resource, po.C.byref(blob)
            ) == 0
            try:
                payload = blob.to_bytes()
                if sample_name in ("docs.tar.xz", "docs-crc64.xz", "docs.tar.txz"):
                    assert payload == (REPO / "tests" / "formats" / "tar" / "fixtures" / "docs.tar").read_bytes()
                else:
                    manifest_name = "no-extension" if sample_name.endswith(".bin") else sample_name[:-3]
                    manifest_path = REPO / "tests" / "formats" / "xz" / "expected" / f"{manifest_name}.json"
                    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))["tree"][0]
                    assert len(payload) == manifest["size"]
                    assert hashlib.sha256(payload).hexdigest() == manifest["sha256"]
            finally:
                peare_lib._lib.peare_blob_free(po.C.byref(blob))
        finally:
            peare_lib._lib.peare_resource_destroy(resource)

        if sample_name == "docs.tar.txz":
            resource = po._HANDLE()
            assert peare_lib._lib.peare_opener_open_resource_at(
                opener._h, 0, 0, po.C.byref(resource)
            ) == 0
            try:
                source = po._HANDLE()
                assert peare_lib._lib.peare_resource_get_source(
                    resource, po.C.byref(source)
                ) == 0
                child = peare_lib._open_source(source)
                peare_lib._lib.peare_source_destroy(source)
                assert child is not None
                with child:
                    assert child.container_format() == "TAR"
            finally:
                peare_lib._lib.peare_resource_destroy(resource)
