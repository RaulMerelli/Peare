"""Test real BZIP2 files through the public Opener ABI."""

import bz2
import io
import os
from pathlib import Path
import tarfile

import pytest
import peare_opener as po


DEFAULT_ROOT = Path(
    Path.home() / "Documents" / "inerente a" / "Peare" / "Peare-font-artifacts-20261102"
)


def test_real_bzip2_tar_samples_open_and_decompress(peare_lib):
    root = Path(os.environ.get("PEARE_BZIP2_SAMPLE_ROOT", DEFAULT_ROOT))
    if not root.is_dir():
        pytest.skip("Set PEARE_BZIP2_SAMPLE_ROOT to the local BZIP2 sample folder")
    samples = sorted(root.rglob("*.tar.bz2"))
    assert samples, f"No .tar.bz2 samples found under {root}"

    for path in samples:
        opener = peare_lib.open_file(str(path))
        assert opener is not None, path
        with opener:
            assert opener.container_format() == "BZIP2", path
            assert opener.folder_count() == 1, path
            assert opener.resource_count(0) == 1, path
            resource = po._HANDLE()
            status = peare_lib._lib.peare_opener_open_resource_at(
                opener._h, 0, 0, po.C.byref(resource)
            )
            assert status == 0, path
            try:
                context = po._Context()
                assert peare_lib._lib.peare_resource_get_context(
                    resource, po.C.byref(context)
                ) == 0, path
                expected_size = int(context.data_size)
                peare_lib._lib.peare_resource_context_free(po.C.byref(context))

                blob = po._Blob()
                assert peare_lib._lib.peare_resource_get_payload(
                    resource, po.C.byref(blob)
                ) == 0, path
                try:
                    payload = blob.to_bytes()
                    assert len(payload) == expected_size, path
                finally:
                    peare_lib._lib.peare_blob_free(po.C.byref(blob))
            finally:
                peare_lib._lib.peare_resource_destroy(resource)

        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:") as archive:
            assert archive.getmembers(), path
        assert payload == bz2.decompress(path.read_bytes()), path
