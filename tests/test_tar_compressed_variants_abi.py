"""Check compressed TAR filename variants through the public Opener ABI."""

import os
import shutil
from pathlib import Path

import pytest
import peare_opener as po


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "formats" / "tar-compressed" / "fixtures"
TAR = ROOT / "tests" / "formats" / "tar" / "fixtures" / "docs.tar"
SAMPLES = (("docs.tgz", "GZIP"), ("docs.tbz", "BZIP2"), ("docs.tbz2", "BZIP2"))


@pytest.mark.parametrize(("name", "format_name"), SAMPLES)
def test_compressed_tar_variants_match_tar_source(peare_lib, name, format_name):
    opener = peare_lib.open_file(str(FIXTURES / name))
    assert opener is not None
    with opener:
        assert opener.container_format() == format_name
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
                assert blob.to_bytes() == TAR.read_bytes()
            finally:
                peare_lib._lib.peare_blob_free(po.C.byref(blob))

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


def test_taz_extension_alias_opens_upstream_tar_compress_sample(peare_lib, tmp_path):
    default_root = (
        Path.home() / "Documents" / "inerente a" / "Peare" /
        "compress-samples" / "libarchive"
    )
    root = Path(os.environ.get("PEARE_COMPRESS_SAMPLE_ROOT", default_root))
    source_path = root / "test_compat_mac-1.tar.Z"
    if not source_path.is_file():
        pytest.skip(f"Missing upstream TAR.Z sample: {source_path}")
    alias = tmp_path / "test_compat_mac-1.taz"
    shutil.copyfile(source_path, alias)

    opener = peare_lib.open_file(str(alias))
    assert opener is not None
    with opener:
        assert opener.container_format() == "UNIX_COMPRESS"
        assert opener.folder_count() == 1
        assert opener.resource_count(0) == 1
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
