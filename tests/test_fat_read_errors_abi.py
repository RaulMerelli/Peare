import ctypes as C
import hashlib
import os
import struct
from pathlib import Path

import pytest

from peare_opener import _HANDLE
from test_callback_source_abi import READ, RELEASE
from test_tibx_abi import _directory_source, _open_source_with_password


class PartitionSource:
    def __init__(self, lib, path, start, length, name=b"partition.img"):
        self.file = path.open("rb")
        self.start = start
        self.length = length
        self.fail_offset = None
        self.failed = False
        self.reader = READ(self.read)
        self.release = RELEASE(self.close)
        self.handle = _HANDLE()
        create = lib._lib.peare_source_create_callback
        create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, READ, RELEASE,
                           C.POINTER(_HANDLE)]
        create.restype = C.c_int
        assert create(
            length, name, None, self.reader, self.release,
            C.byref(self.handle)) == 0

    def read(self, context, offset, buffer, capacity, out_read):
        if offset == self.fail_offset:
            self.failed = True
            return 13
        self.file.seek(self.start + offset)
        data = self.file.read(min(capacity, 127, self.length - offset))
        C.memmove(buffer, data, len(data))
        out_read[0] = len(data)
        return 0

    def close(self, context):
        self.file.close()


def test_fat12_without_type_label(peare_lib):
    sample = os.environ.get("PEARE_FAT_UNLABELLED_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_UNLABELLED_IMAGE to the independent FAT12 sample")
    path = Path(sample)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "c0546284fa6f751e108c6045a339297adc3bc14fbeb252ee2a957e19ef6d51cd"
    )
    source = PartitionSource(peare_lib, path, 0, path.stat().st_size, b"partition.bin")
    opener = None
    try:
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and opener is not None
        assert opener.container_format() == "FAT"
        rows = opener.normalized_tree(max_depth=1)
        item = next(row for row in rows if row["path"] == "AK.DRW")
        assert item["size"] == 27083
        assert item["sha256"] == "16e2b16abbd73a40de03b6da9196317222772aa74feb21a3de8f60662920160b"
    finally:
        if opener is not None:
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)


def test_fat_supplementary_unicode_filename(peare_lib):
    samples = os.environ.get("PEARE_ACRONIS_SAMPLES")
    path = Path(samples) / "unicode-fat-pyfatfs.img" if samples else None
    if not path or not path.is_file():
        pytest.skip("The independent Unicode FAT sample is not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "4ef7cb9db8f15eab096c4839f80f46e48933e7efd75224603cbf7eda85420ddb")
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        rows = opener.normalized_tree(recurse_containers=False)
        item = next(row for row in rows if row["path"] == "name_\U0001f680.txt")
        assert item["size"] == 23
        assert item["sha256"] == "bc291f0b60c447c57657d25a67e91459bd1eb810dae688bfb463bf8c16df3edb"


def sample_partition():
    sample = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE")
    if not sample:
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to an independent MikroTik raw disk")
    path = Path(sample)
    if not path.is_file():
        pytest.skip("Independent MikroTik raw disk is not available")
    with path.open("rb") as image:
        digest = hashlib.sha256()
        for block in iter(lambda: image.read(1024 * 1024), b""):
            digest.update(block)
        assert digest.hexdigest() == "afa12eb5d17474961f9d1df5ba4083ab5ab6c91ad665bf1d2b4ddb6d4f03e693"
        image.seek(0)
        mbr = image.read(512)
        start, sectors = struct.unpack_from("<II", mbr, 446 + 8)
        start *= 512
        image.seek(start)
        boot = image.read(512)
        bps = struct.unpack_from("<H", boot, 11)[0]
        spc = boot[13]
        reserved = struct.unpack_from("<H", boot, 14)[0]
        fats = boot[16]
        fat_size = struct.unpack_from("<I", boot, 36)[0]
        root_cluster = struct.unpack_from("<I", boot, 44)[0]
        data_start = (reserved + fats * fat_size) * bps
        root = data_start + (root_cluster - 2) * spc * bps
        image.seek(start + root)
        root_bytes = image.read(spc * bps)
        efi = next(root_bytes[i:i + 32] for i in range(0, len(root_bytes), 32)
                   if root_bytes[i:i + 11] == b"EFI        " and root_bytes[i + 11] & 0x10)
        cluster = (struct.unpack_from("<H", efi, 20)[0] << 16) | struct.unpack_from("<H", efi, 26)[0]
        return path, start, sectors * 512, reserved * bps, root, data_start + (cluster - 2) * spc * bps


@pytest.mark.parametrize("target", ["table", "root", "subdirectory"])
def test_fat_metadata_read_failure_is_not_empty_directory(peare_lib, target):
    path, start, length, table, root, subdirectory = sample_partition()
    source = PartitionSource(peare_lib, path, start, length)
    opener = None
    child = None
    nested = _HANDLE()
    try:
        source.fail_offset = {"table": table, "root": root}.get(target)
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        if target == "subdirectory":
            assert status == 0 and opener is not None
            nested = _directory_source(opener, "EFI")
            source.fail_offset = subdirectory
            status, child = _open_source_with_password(peare_lib, nested, None)
        assert source.failed
        assert status != 0
        assert child is None
        if target != "subdirectory":
            assert opener is None
    finally:
        if child is not None:
            child.close()
        if nested:
            peare_lib._lib.peare_source_destroy(nested)
        if opener is not None:
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
