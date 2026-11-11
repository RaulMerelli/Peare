"""Check independent CPIO samples through the public Opener ABI."""

import ctypes as C
from collections import Counter
import os
from pathlib import Path
import shutil
import subprocess

import pytest
import peare_opener as po
from external_tools import enabled, find


DEFAULT_ROOT = Path.home() / "Documents" / "inerente a" / "Peare" / "cpio-samples"
SAMPLE_NAMES = (
    "ascii-3.0-1.src.cpio",
    "cinema.cpi",
    "pcmcia",
    "skeleton",
    "skeleton2.cpio",
    "libarchive/test_read_format_cpio_afio.cpio",
    "libarchive/test_read_format_cpio_bin_le.cpio",
    "libarchive/test_read_format_cpio_bin_be.cpio",
)
SAMPLE_MAGICS = {
    "ascii-3.0-1.src.cpio": b"070701",
    "cinema.cpi": b"070707",
    "pcmcia": b"070702",
    "skeleton": b"070702",
    "skeleton2.cpio": b"\x71\xc7",
    "libarchive/test_read_format_cpio_afio.cpio": b"070707",
    "libarchive/test_read_format_cpio_bin_le.cpio": b"\xc7\x71",
    "libarchive/test_read_format_cpio_bin_be.cpio": b"\x71\xc7",
}


def _entries(peare_lib, opener):
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
                payload = b""
                if size:
                    blob = po._Blob()
                    assert peare_lib._lib.peare_resource_get_payload(
                        resource, C.byref(blob)
                    ) == 0, path
                    try:
                        payload = blob.to_bytes()
                    finally:
                        peare_lib._lib.peare_blob_free(C.byref(blob))
                entries.append((path, size, payload))
            finally:
                peare_lib._lib.peare_resource_destroy(resource)
    return entries


@pytest.mark.parametrize("relative_name", SAMPLE_NAMES)
def test_cpio_samples_match_libarchive(peare_lib, relative_name):
    root = Path(os.environ.get("PEARE_CPIO_SAMPLE_ROOT", DEFAULT_ROOT))
    sample = root / relative_name
    if not sample.is_file():
        pytest.skip(f"Missing external CPIO sample: {sample}")
    assert sample.read_bytes()[: len(SAMPLE_MAGICS[relative_name])] == SAMPLE_MAGICS[relative_name]
    tar = ((os.environ.get("PEARE_LIBARCHIVE_TAR") or find("bsdtar", "tar"))
           if enabled() else None)
    if not tar:
        pytest.skip("Set PEARE_LIBARCHIVE_TAR to bsdtar for the independent oracle")

    expected = subprocess.run([tar, "-tf", str(sample)], check=True, capture_output=True)
    listed_names = [
        line.decode("utf-8", "replace").rstrip("/").removeprefix("./")
        for line in expected.stdout.splitlines() if line
    ]
    verbose = subprocess.run([tar, "-tvf", str(sample)], check=True, capture_output=True)
    verbose_lines = verbose.stdout.splitlines()
    assert len(verbose_lines) == len(listed_names)
    expected_names = [name for name, row in zip(listed_names, verbose_lines) if not row.startswith(b"d")]
    expected_regular = {
        name for name, row in zip(listed_names, verbose_lines) if row.startswith(b"-")
    }
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "CPIO"
        actual = _entries(peare_lib, opener)
    assert Counter(entry[0] for entry in actual) == Counter(expected_names)

    for name, size, payload in actual[:20]:
        if not size or name not in expected_regular:
            continue
        reference = subprocess.run(
            [tar, "-xOf", str(sample), name], check=True, capture_output=True
        ).stdout
        assert payload == reference, name


def test_debian_initrd_opens_as_nested_cpio(peare_lib):
    root = Path(os.environ.get("PEARE_CPIO_SAMPLE_ROOT", DEFAULT_ROOT))
    sample = root / "debian-bookworm-initrd-amd64.gz"
    if not sample.is_file():
        pytest.skip(f"Missing official Debian initrd sample: {sample}")
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "GZIP"
        nested = None
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
                        nested = peare_lib._open_source(source)
                        peare_lib._lib.peare_source_destroy(source)
                        break
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)
            if nested:
                break
        assert nested is not None
        with nested:
            assert nested.container_format() == "CPIO"
            assert nested.folder_count() > 0
            assert sum(nested.resource_count(i) for i in range(nested.folder_count())) > 0
