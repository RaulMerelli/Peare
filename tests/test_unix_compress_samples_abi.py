"""Check independent Unix compress samples through the public Opener ABI."""

import ctypes as C
from collections import Counter
import os
from pathlib import Path
import shutil
import subprocess

import pytest
from external_tools import enabled, find
import peare_opener as po


DEFAULT_ROOT = Path(
    Path.home() / "Documents" / "inerente a" / "Peare" / "compress-samples" / "libarchive"
)
SAMPLES = (
    "test_compat_mac-1.tar.Z",
    "test_compat_mac-2.tar.Z",
)


def _open_child(peare_lib, opener):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = po._HANDLE()
            assert peare_lib._lib.peare_opener_open_resource_at(
                opener._h, folder, index, C.byref(resource)
            ) == 0
            try:
                source = po._HANDLE()
                assert peare_lib._lib.peare_resource_get_source(
                    resource, C.byref(source)
                ) == 0
                if source:
                    child = peare_lib._open_source(source)
                    peare_lib._lib.peare_source_destroy(source)
                    if child:
                        return child
            finally:
                peare_lib._lib.peare_resource_destroy(resource)
    return None


def _read_entries(peare_lib, opener):
    entries = []
    for folder in range(opener.folder_count()):
        folder_path = opener.folder_type(folder).replace("\x1f", "/").strip("/")
        for index in range(opener.resource_count(folder)):
            resource = po._HANDLE()
            assert peare_lib._lib.peare_opener_open_resource_at(
                opener._h, folder, index, C.byref(resource)
            ) == 0
            try:
                context = po._Context()
                assert peare_lib._lib.peare_resource_get_context(
                    resource, C.byref(context)
                ) == 0
                name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                kind = context.type_utf8.to_bytes().decode("utf-8", "replace")
                size = int(context.data_size)
                peare_lib._lib.peare_resource_context_free(C.byref(context))
                if kind.endswith("_DIR"):
                    continue
                path = f"{folder_path}/{name}" if folder_path else name
                blob = po._Blob()
                assert peare_lib._lib.peare_resource_get_payload(
                    resource, C.byref(blob)
                ) == 0, path
                try:
                    entries.append((path, size, blob.to_bytes()))
                finally:
                    peare_lib._lib.peare_blob_free(C.byref(blob))
            finally:
                peare_lib._lib.peare_resource_destroy(resource)
    return entries


@pytest.mark.parametrize("sample_name", SAMPLES)
def test_unix_compress_tar_samples_match_libarchive(peare_lib, sample_name):
    root = Path(os.environ.get("PEARE_COMPRESS_SAMPLE_ROOT", DEFAULT_ROOT))
    sample = root / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external .Z sample: {sample}")
    tar = ((os.environ.get("PEARE_LIBARCHIVE_TAR") or find("bsdtar", "tar"))
           if enabled() else None)
    if not tar:
        pytest.skip("Set PEARE_LIBARCHIVE_TAR to bsdtar for the independent oracle")

    listing = subprocess.run([tar, "-tf", str(sample)], check=True, capture_output=True)
    listed_names = [
        line.decode("utf-8", "replace").rstrip("/").removeprefix("./")
        for line in listing.stdout.splitlines() if line
    ]
    verbose = subprocess.run([tar, "-tvf", str(sample)], check=True, capture_output=True)
    verbose_lines = verbose.stdout.splitlines()
    assert len(verbose_lines) == len(listed_names)
    expected = [
        name for name, row in zip(listed_names, verbose_lines)
        if row.startswith((b"-", b"l"))
    ]

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "UNIX_COMPRESS"
        child = _open_child(peare_lib, opener)
        assert child is not None
        with child:
            assert child.container_format() == "TAR"
            actual = _read_entries(peare_lib, child)

    if sample_name == "test_compat_mac-1.tar.Z":
        # The TAR reader does not parse this fixture's extended long paths.
        expected = [name for name in expected if "/" not in name]
        actual = [entry for entry in actual if "/" not in entry[0]]
    assert Counter(entry[0] for entry in actual) == Counter(expected)
    for name, _, payload in actual:
        reference = subprocess.run(
            [tar, "-xOf", str(sample), name], check=True, capture_output=True
        ).stdout
        assert payload == reference, name


def test_unix_compress_multivolume_fixture_matches_libarchive_reference(peare_lib):
    root = Path(os.environ.get("PEARE_COMPRESS_SAMPLE_ROOT", DEFAULT_ROOT))
    sample = root / "test_read_filter_compress_multi_volume.Z"
    if not sample.is_file():
        pytest.skip(f"Missing external multi-volume .Z sample: {sample}")
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "UNIX_COMPRESS"
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
                payload = blob.to_bytes()
            finally:
                peare_lib._lib.peare_blob_free(C.byref(blob))
        finally:
            peare_lib._lib.peare_resource_destroy(resource)
    assert len(payload) == 66560
    assert not any(payload)
