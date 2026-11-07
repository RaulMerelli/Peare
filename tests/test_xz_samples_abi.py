"""Test independent XZ samples through the public Opener ABI."""

import os
from pathlib import Path

import pytest
import peare_opener as po


REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = REPO / "tests" / "formats" / "xz" / "fixtures"


@pytest.mark.parametrize(
    ("sample_name", "payload_paths"),
    [
        ("docs.tar.xz", (REPO / "tests" / "formats" / "tar" / "fixtures" / "docs.tar",)),
        ("docs-crc64.xz", (REPO / "tests" / "formats" / "tar" / "fixtures" / "docs.tar",)),
        ("README-sha256.xz", (REPO / "README.md",)),
        ("docs.tar.txz", (REPO / "tests" / "formats" / "tar" / "fixtures" / "docs.tar",)),
        ("README.md.xz", (REPO / "README.md",)),
        (
            "concatenated.xz",
            (REPO / "README.md", REPO / "tests" / "formats" / "tar" / "fixtures" / "docs.tar"),
        ),
        ("no-extension.bin", (REPO / "README.md",)),
    ],
)
def test_independently_created_xz_samples_match_original_payload(
    peare_lib, sample_name, payload_paths
):
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
                assert blob.to_bytes() == b"".join(path.read_bytes() for path in payload_paths)
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
