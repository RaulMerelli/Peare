"""Check standalone LZMA samples through the public Opener ABI."""

from pathlib import Path
import shutil

import ctypes as C
import peare_opener as po


REPO = Path(__file__).resolve().parents[1]
FIXTURES = REPO / "tests" / "formats" / "lzma" / "fixtures"


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


def test_lzma_unknown_size_matches_independent_encoder_source(peare_lib):
    sample = FIXTURES / "README.md.lzma"
    expected = REPO / "README.md"
    assert sample.is_file()
    assert expected.is_file()

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "LZMA"
        assert _payload(peare_lib, opener) == expected.read_bytes()


def test_lzma_extension_does_not_override_archive_signature(peare_lib, tmp_path):
    sample = REPO / "tests" / "formats" / "tar" / "fixtures" / "docs.tar"
    renamed = tmp_path / "docs.lzma"
    shutil.copyfile(sample, renamed)

    opener = peare_lib.open_file(str(renamed))
    assert opener is not None
    with opener:
        assert opener.container_format() == "TAR"
