import ctypes as C
import hashlib
import os
import struct
from pathlib import Path

import pytest

from peare_opener import CONTAINER_NAMES, _Context, _HANDLE


def test_real_mbr_partition_sources_survive_parent(peare_lib):
    default = Path(__file__).resolve().parents[1] / "tmp" / "lot3-chr-sample" / "chr-7.20.7-arm64.img"
    path = Path(os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE", str(default)))
    if not path.is_file():
        pytest.skip("Independent MikroTik raw disk is not available")
    digest = hashlib.sha256()
    with path.open("rb") as raw:
        for block in iter(lambda: raw.read(1024 * 1024), b""):
            digest.update(block)
        assert digest.hexdigest() == "afa12eb5d17474961f9d1df5ba4083ab5ab6c91ad665bf1d2b4ddb6d4f03e693"
        raw.seek(0)
        mbr = raw.read(512)
        assert mbr[510:512] == b"\x55\xaa"
        spans = []
        for index in range(4):
            start, count = struct.unpack_from("<II", mbr, 0x1be + 16 * index + 8)
            if mbr[0x1be + 16 * index + 4] and start and count:
                spans.append((start * 512, count * 512))
        assert len(spans) == 2

        opener = peare_lib.open_file(str(path))
        assert opener is not None and opener.container_format() == "RAW_DISK"
        resources = []
        sources = []
        try:
            assert sum(opener.resource_count(i) for i in range(opener.folder_count())) == len(spans)
            for folder in range(opener.folder_count()):
                for index in range(opener.resource_count(folder)):
                    resource = _HANDLE()
                    assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(resource)) == 0
                    resources.append(resource)
                    context = _Context()
                    assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
                    try:
                        span = (int(context.data_offset), int(context.data_size))
                    finally:
                        opener._L.peare_resource_context_free(C.byref(context))
                    assert span in spans
                    source = _HANDLE()
                    assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
                    sources.append((source, span))
            assert sorted(span for _, span in sources) == sorted(spans)
            opener.close()
            for resource, (source, (start, size)) in zip(resources, sources):
                for offset in (0, 511, size // 2, size - 1024):
                    count = min(1024, size - offset)
                    raw.seek(start + offset)
                    assert peare_lib.read_resource(resource, offset, count) == raw.read(count)
                nested = peare_lib._open_source(source)
                assert nested is not None
                try:
                    assert nested.container_format() in ("FAT", "EXT")
                finally:
                    nested.close()
        finally:
            opener.close()
            for source, _ in sources:
                peare_lib._lib.peare_source_destroy(source)
            for resource in resources:
                peare_lib._lib.peare_resource_destroy(resource)


def test_real_lumia_vhdx_partition_source_survives_parent(peare_lib):
    image = os.environ.get("PEARE_VHDX_TEST_IMAGE")
    if not image or not Path(image).is_file():
        pytest.skip("Set PEARE_VHDX_TEST_IMAGE to the documented Lumia VHDX sample")
    opener = peare_lib.open_file(image)
    assert opener is not None and opener.container_format() == "VHDX"
    resource = _HANDLE()
    source = _HANDLE()
    try:
        assert sum(opener.resource_count(i) for i in range(opener.folder_count())) == 38
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                candidate = _HANDLE()
                assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(candidate)) == 0
                context = _Context()
                assert opener._L.peare_resource_get_context(candidate, C.byref(context)) == 0
                try:
                    is_ntfs = CONTAINER_NAMES.get(context.container_format) == "NTFS"
                finally:
                    opener._L.peare_resource_context_free(C.byref(context))
                if is_ntfs:
                    resource = candidate
                    break
                opener._L.peare_resource_destroy(candidate)
            if resource:
                break
        assert resource
        assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
        boot = peare_lib.read_resource(resource, 0, 512)
        assert len(boot) == 512 and boot[3:11] == b"NTFS    "
        opener.close()
        assert peare_lib.read_resource(resource, 0, 512) == boot
        child = peare_lib._open_source(source)
        assert child is not None
        try:
            assert child.container_format() == "NTFS"
        finally:
            child.close()
    finally:
        opener.close()
        if source:
            peare_lib._lib.peare_source_destroy(source)
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
