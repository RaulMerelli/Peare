"""Synthetic TIBX vectors constructed for ABI testing, not authentic backups."""

import ctypes as C
import os
import struct
from pathlib import Path

import pytest

from peare_opener import _Context, _HANDLE
from test_tibx_abi import _directory_source, _open_source_with_password


def _crc32c(data):
    crc = 0xffffffff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc ^ 0xffffffff


def _page(kind, payload):
    page = bytearray(4096)
    page[:4] = bytes((ord("A"), kind, 0, 0))
    page[8:8 + len(payload)] = payload
    struct.pack_into(">I", page, 4, _crc32c(page))
    return page


def _commit_crc(page):
    crc = 0
    for i, byte in enumerate(page[:512]):
        if 4 <= i < 8 or 72 <= i < 80:
            byte = 0
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x95ac9329ac4bc9b5 if crc & 1 else 0)
    return crc


def _varint(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def _tree(cells, key_size, value_size):
    assert len(cells) <= 24
    payload = bytearray((len(cells),))
    payload.extend(((1 << len(cells)) - 1).to_bytes(3, "big"))
    for key, value in cells:
        if not key_size:
            payload.extend(_varint(len(key)))
            payload.extend(_varint(len(value)))
        else:
            assert len(key) == key_size and len(value) == value_size
        payload.extend(key)
        payload.extend(value)
    superblock = bytearray(376)
    superblock[:4] = b"L-SB"
    struct.pack_into(">II", superblock, 16, key_size, value_size)
    struct.pack_into(">H", superblock, 346, len(cells))
    struct.pack_into(">I", superblock, 348, len(payload))
    return superblock + payload


def _section(data):
    return struct.pack(">I", len(data)) + data + bytes((-len(data)) % 4)


def _synthetic_archive(stream_flags=3, secondary_position=None, link_case=None):
    stream_id = 41
    def item(slice_id, name):
        primary = bytes((0, stream_flags)) + struct.pack(">QQ", stream_id, 4)
        secondary = bytes((7, 0)) + struct.pack(">QQ", 999, 64)
        descriptors = [primary]
        if secondary_position == "before":
            descriptors.insert(0, secondary)
        elif secondary_position == "after":
            descriptors.append(secondary)
        return (struct.pack(">I", slice_id) + b"\0\3\0" + name,
                bytes((len(descriptors) - 1,)) + b"".join(descriptors))
    if link_case:
        alias_slice = 2 if link_case == "different_slice" else 3
        imap = _tree([item(3, b"disk1/part1"), item(alias_slice, b"part/abc")], 0, 0)
        names = [(3, b"disk1/part1"), (alias_slice, b"part/abc")]
        if link_case == "stale":
            names[1] = (2, b"part/abc")
        names.sort(key=lambda v: (stream_id + (link_case == "different_id" and v[1] == b"part/abc"),
                                  -v[0], len(v[1]), v[1]))
        nlink = _tree([(struct.pack(">QI", stream_id + (link_case == "different_id" and name == b"part/abc"), version) + name, b"")
                       for version, name in names], 0, 0)
        if link_case == "tombstone":
            alias_index = next(i for i, (_, name) in enumerate(names) if name == b"part/abc")
            nlink[377:380] = (3 & ~(1 << alias_index)).to_bytes(3, "big")
        if link_case == "missing":
            nlink = b""
        elif link_case == "invalid":
            nlink[0] ^= 1
    else:
        imap = _tree([item(2, b"disk1/part1"), item(3, b"disk1/part2")], 0, 0)
        nlink = b""

    def extent(slice_id, segment_id):
        key = struct.pack(">QQ", stream_id, 0) + b"\0\0\4" + struct.pack(">IQ", slice_id, 0)
        return key, struct.pack(">QH", segment_id, 0xffff)

    dmap = _tree([extent(2, 51), extent(3, 52)], 31, 10)
    segments = _tree([
        (struct.pack(">Q", 51), struct.pack(">Q", (1 << 56) | 4) + bytes(24)),
        (struct.pack(">Q", 52), struct.pack(">Q", (1 << 56) | 5) + bytes(24)),
    ], 8, 32)
    point = bytearray(132)
    struct.pack_into(">I", point, 48, 2)
    slices = _tree([(struct.pack(">I", 3), point)], 4, 132)

    header = bytearray(1024)
    header[:4] = b"ARCH"
    struct.pack_into(">H", header, 8, 8)
    header[32:48] = bytes(range(16))
    for section in (imap, dmap, segments, b"", nlink, slices):
        header.extend(_section(section))
    struct.pack_into(">I", header, 4, len(header))
    assert len(header) <= 4088

    commit = _page(2, b"ARCI")
    struct.pack_into(">H", commit, 12, 8)
    struct.pack_into(">IIQ", commit, 16, 2, 4096, 3 * 4096)
    struct.pack_into(">Q", commit, 32, 1)
    commit[136:152] = bytes(range(16))
    struct.pack_into(">Q", commit, 72, _commit_crc(commit))
    struct.pack_into(">I", commit, 4, 0)
    struct.pack_into(">I", commit, 4, _crc32c(commit))

    def segment(data):
        payload = bytearray(36 + len(data))
        payload[:4] = b"SG\0\1"
        struct.pack_into(">II", payload, 4, len(data), len(data))
        payload[36:] = data
        return _page(255, payload)

    arch = _page(1, header)
    return bytes(arch + _page(1, b"ARCH") + commit + arch + segment(b"old!") + segment(b"new!"))


@pytest.mark.parametrize("case,alias_visible", [
    ("valid", False), ("missing", True), ("stale", True),
    ("tombstone", True), ("different_slice", True), ("different_id", True), ("invalid", True),
])
def test_synthetic_tibx_link_index_controls_alias_display(peare_lib, tmp_path, case, alias_visible):
    path = tmp_path / "synthetic-tibx-links.tibx"
    path.write_bytes(_synthetic_archive(link_case=case))
    opener = peare_lib.open_file(str(path))
    assert opener is not None
    try:
        names = set()
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource, context = _HANDLE(), _Context()
                assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(resource)) == 0
                try:
                    assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
                    try:
                        names.add(context.identifier_utf8.to_bytes().decode("utf-8"))
                    finally:
                        opener._L.peare_resource_context_free(C.byref(context))
                finally:
                    opener._L.peare_resource_destroy(resource)
        assert "disk1" in names
        assert ("part" in names) == alias_visible
        if case in ("missing", "different_slice"):
            for directory_name, resource_name, expected in (
                    ("disk1", b"part1", b"new!"),
                    ("part", b"abc", b"old!" if case == "different_slice" else b"new!")):
                source = _directory_source(opener, directory_name)
                try:
                    status, child = _open_source_with_password(peare_lib, source, None)
                finally:
                    peare_lib._lib.peare_source_destroy(source)
                assert status == 0 and child is not None
                try:
                    resource = _HANDLE()
                    find = child._L.peare_opener_find_resource
                    find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
                    find.restype = C.c_int
                    assert find(child._h, b"TIBX_VOLUME", resource_name, None, C.byref(resource)) == 0
                    try:
                        assert peare_lib.read_resource(resource, 0, 4) == expected
                    finally:
                        child._L.peare_resource_destroy(resource)
                finally:
                    child.close()
    finally:
        opener.close()


def test_real_tibx_mbr_linked_aliases_are_not_listed_twice(peare_lib):
    samples = os.environ.get("PEARE_ACRONIS_SAMPLES")
    if not samples:
        pytest.skip("Set PEARE_ACRONIS_SAMPLES for the independently acquired sample")
    root = peare_lib.open_file(str(Path(samples) / "t1_disk_mbr.tibx"))
    assert root is not None
    try:
        names = set()
        for folder in range(root.folder_count()):
            for index in range(root.resource_count(folder)):
                resource, context = _HANDLE(), _Context()
                assert root._L.peare_opener_open_resource_at(root._h, folder, index, C.byref(resource)) == 0
                try:
                    assert root._L.peare_resource_get_context(resource, C.byref(context)) == 0
                    try:
                        names.add(context.identifier_utf8.to_bytes().decode("utf-8"))
                    finally:
                        root._L.peare_resource_context_free(C.byref(context))
                finally:
                    root._L.peare_resource_destroy(resource)
        assert "disk1" in names
        assert "part" not in names
        source = _directory_source(root, "disk1")
        try:
            status, disk = _open_source_with_password(peare_lib, source, None)
        finally:
            peare_lib._lib.peare_source_destroy(source)
        assert status == 0 and disk is not None
        try:
            find = disk._L.peare_opener_find_resource
            find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
            find.restype = C.c_int
            for name in (b"part1", b"part2"):
                resource = _HANDLE()
                assert find(disk._h, b"TIBX_VOLUME", name, None, C.byref(resource)) == 0
                disk._L.peare_resource_destroy(resource)
        finally:
            disk.close()
    finally:
        root.close()


@pytest.mark.parametrize("stream_flags", [0, 1, 2, 3, 0x80, 0xff])
@pytest.mark.parametrize("secondary_position", [None, "before", "after"])
def test_synthetic_tibx_same_stream_id_distinct_item_slices(peare_lib, tmp_path, stream_flags,
                                                            secondary_position):
    path = tmp_path / "synthetic-distinct-item-slices.tibx"
    path.write_bytes(_synthetic_archive(stream_flags, secondary_position))
    root = peare_lib.open_file(str(path))
    assert root is not None
    source = _directory_source(root, "disk1")
    try:
        status, directory = _open_source_with_password(peare_lib, source, None)
    finally:
        peare_lib._lib.peare_source_destroy(source)
        root.close()
    assert status == 0 and directory is not None
    try:
        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        for name, expected in ((b"part1", b"old!"), (b"part2", b"new!")):
            resource = _HANDLE()
            assert find(directory._h, b"TIBX_VOLUME", name, None, C.byref(resource)) == 0
            try:
                context = _Context()
                assert peare_lib._lib.peare_resource_get_context(resource, C.byref(context)) == 0
                try:
                    assert context.data_size == len(expected)
                finally:
                    peare_lib._lib.peare_resource_context_free(C.byref(context))
                assert peare_lib.read_resource(resource, 0, 4) == expected
            finally:
                peare_lib._lib.peare_resource_destroy(resource)
    finally:
        directory.close()
