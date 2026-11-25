"""Synthetic grammar vectors; these are not authentic TIB samples."""

import ctypes as C
import struct
import zlib

import pytest

from peare_opener import _HANDLE
from test_tibx_abi import _directory_source, _open_source_with_password


def _record(tag, payload):
    encoder = zlib.compressobj(wbits=-15)
    compressed = encoder.compress(payload) + encoder.flush()
    return bytes([tag]) + compressed + struct.pack("<I", zlib.crc32(payload))


def _text(value):
    raw = value.encode("utf-16le")
    return struct.pack("<I", len(raw) // 2) + raw


def _item(name, offset):
    return _text(name) + _text("") + struct.pack("<QIQQIII", 0, 0, offset, 0, 0, 0, 0)


def _type6(kind, extra=b""):
    return struct.pack("<IIIII", 20 + len(extra), kind, 0, 0, 0) + extra


def _archive(fault=None):
    image = bytearray(struct.pack("<IHHQII", 0xa2b924ce, 24, 0, 1, 2, 1))
    children = []

    def add(name, chunks):
        offset = len(image) - 24
        image.extend(_record(ord("f"), b""))
        for tag, payload in chunks:
            record = bytearray(_record(tag, payload))
            if fault == "crc" and name == "file" and tag == 4:
                record[-1] ^= 1
            image.extend(record)
        children.append((name, offset))

    directory_offset = len(image) - 24
    image.extend(_record(ord("f"), b""))
    image.extend(_record(1, struct.pack("<I", 0x10)))
    file_type = _type6(128 if fault == "conflict" else 0)
    add("file", [(6, file_type), (4, b"hello")])
    backup = struct.pack("<IIQI", 9, 0, 10, 0) + struct.pack("<Q", 3) + b"xy"
    add("sparse", [(2, backup)])
    target = "destination".encode("utf-16le")
    link = struct.pack("<H", len(target) // 2) + target
    if fault == "link":
        link = struct.pack("<H", 100) + target
    add("link", [(6, _type6(5, link))])
    add("device", [(6, _type6(1, struct.pack("<II", 12, 34)))])
    add("fifo", [(6, _type6(3))])
    add("socket", [(6, _type6(4))])
    tree = bytearray(struct.pack("<II", 1, len(children)))
    for name, offset in children:
        tree.extend(struct.pack("<I", 0))
        tree.extend(_item("\\" + name, offset))
    tree.extend(_item("folder", directory_offset))
    metadata_offset = len(image) - 24
    image.extend(_record(ord("e"), struct.pack("<II", 1, 0)))
    image.extend(_record(ord("g"), bytes(tree)))
    image.extend(b"h" + bytes(6) + struct.pack("<Q", metadata_offset))
    image.extend(struct.pack("<I", 0x94e18a2c))
    return image


def _open_folder(peare_lib, path):
    root = peare_lib.open_file(str(path))
    assert root is not None
    source = _directory_source(root, "folder")
    try:
        status, child = _open_source_with_password(peare_lib, source, None)
    finally:
        peare_lib._lib.peare_source_destroy(source)
        root.close()
    return status, child


def _read(peare_lib, opener, kind, name, size):
    find = peare_lib._lib.peare_opener_find_resource
    find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
    find.restype = C.c_int
    resource = _HANDLE()
    assert find(opener._h, kind.encode(), name.encode(), None, C.byref(resource)) == 0
    try:
        return peare_lib.read_resource(resource, 0, size) if size else b""
    finally:
        peare_lib._lib.peare_resource_destroy(resource)


def test_synthetic_legacy_tree_and_special_metadata(peare_lib, tmp_path):
    path = tmp_path / "synthetic-legacy.tib"
    path.write_bytes(_archive())
    status, folder = _open_folder(peare_lib, path)
    assert status == 0 and folder is not None
    try:
        assert _read(peare_lib, folder, "TIB_FILE", "file", 5) == b"hello"
        assert _read(peare_lib, folder, "TIB_FILE", "sparse", 5) == b"\0\0\0xy"
        assert _read(peare_lib, folder, "TIB_SYMLINK_UTF16LE", "link", 22) == "destination".encode("utf-16le")
        assert _read(peare_lib, folder, "TIB_DEVICE_CHAR", "device", 8) == struct.pack("<II", 12, 34)
        assert _read(peare_lib, folder, "TIB_FIFO", "fifo", 0) == b""
        assert _read(peare_lib, folder, "TIB_SOCKET", "socket", 0) == b""
    finally:
        folder.close()


@pytest.mark.parametrize("fault", ["crc", "conflict", "link"])
def test_synthetic_legacy_invalid_item_is_not_silently_empty(peare_lib, tmp_path, fault):
    path = tmp_path / "synthetic-invalid.tib"
    path.write_bytes(_archive(fault))
    status, folder = _open_folder(peare_lib, path)
    try:
        assert status != 0 and folder is None
    finally:
        if folder:
            folder.close()
