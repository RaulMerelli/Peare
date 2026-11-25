"""Synthetic data-map retention vectors; these are not authentic archives."""

import ctypes as C
import struct
import zlib

import pytest

from peare_opener import _HANDLE
from test_callback_source_abi import CallbackSource
from test_tibx_abi import _directory_source, _open_source_with_password
from test_tibx_synthetic_abi import _commit_crc, _crc32c, _page, _section, _tree


def _compact(cells, key_size, value_size):
    if not cells:
        return b""
    assert len(cells) <= 24
    alive = sum(1 << index for index, (_, value) in enumerate(cells) if value is not None)
    out = bytearray([len(cells)]) + alive.to_bytes(3, "big")
    for key, value in cells:
        assert len(key) == key_size
        out.extend(key)
        if value is not None:
            assert len(value) == value_size
            out.extend(value)
    return bytes(out)


def _superblock(key_size, value_size, inline, levels):
    payload = _compact(inline, key_size, value_size)
    out = bytearray(376)
    out[:4] = b"L-SB"
    out[5] = out[6] = len(levels)
    struct.pack_into(">II", out, 16, key_size, value_size)
    for index, page in enumerate(levels):
        struct.pack_into(">Q", out, 24 + index * 32, page * 4096)
    struct.pack_into(">H", out, 346, len(inline))
    struct.pack_into(">I", out, 348, len(payload))
    return bytes(out) + payload


def _leaf(cells, key_size, value_size):
    data = _compact(cells, key_size, value_size)
    header = bytearray(52)
    header[:4] = b"LEAF"
    struct.pack_into(">HII", header, 6, len(cells), len(data), len(data))
    return _page(3, header + data)


def _segment(data):
    payload = bytearray(36 + len(data))
    payload[:4] = b"SG\0\1"
    struct.pack_into(">II", payload, 4, len(data), len(data))
    payload[36:] = data
    return _page(255, payload)


def _zlib_segment(data, case):
    if case == "zlib_raw_deflate":
        compressor = zlib.compressobj(wbits=-15)
        packed = compressor.compress(data) + compressor.flush()
    else:
        packed = zlib.compress(data)
    if case == "zlib_bad_adler":
        packed = packed[:-1] + bytes([packed[-1] ^ 1])
    elif case == "zlib_truncated":
        packed = packed[:-1]
    payload = bytearray(36 + len(packed))
    payload[:4] = b"SG\0\1"
    expanded = len(data) + (case == "zlib_wrong_size")
    struct.pack_into(">II", payload, 4, expanded, len(packed))
    payload[16] = 2
    payload[36:] = packed
    return _page(255, payload)


def _archive(case):
    def key(offset, sequence):
        return struct.pack(">QQ", 41, offset) + b"\0\0\4" + struct.pack(">IQ", 3, sequence)

    first, second = key(0, 1), key(4, 2)
    value = lambda segment: struct.pack(">QH", segment, 0xffff)
    old = [(first, value(51)), (second, value(52))]
    inline = []
    leaves = [old]
    if case == "inline_tombstone":
        inline = [(first, None)]
    elif case == "inline_replacement":
        inline = [(first, value(53))]
    elif case == "inline_last_tombstone":
        inline = [(first, value(53)), (first, None)]
    elif case == "inline_last_replacement":
        inline = [(first, None), (first, value(53))]
    elif case == "level_tombstone":
        leaves = [[(first, None), (second, value(52))], old]

    segment_values = [(struct.pack(">Q", ident), struct.pack(">Q", (1 << 56) | page) + bytes(24))
                      for ident, page in ((51, 4), (52, 5), (53, 6))]
    if case == "physical_segment_missing":
        segment_values[0] = (struct.pack(">Q", 51), struct.pack(">Q", (1 << 56) | 100) + bytes(24))
    first_segment = _zlib_segment(b"AAAA", case) if case.startswith("zlib_") else _segment(b"AAAA")
    pages = [first_segment, _segment(b"BBBB"), _segment(b"CCCC")]
    pages.extend(_leaf(leaf, 31, 10) for leaf in leaves)
    dmap = _superblock(31, 10, inline, list(range(7, 7 + len(leaves))))
    if case == "missing_segment":
        segment_leaf = 7 + len(leaves)
        pages.append(_leaf(segment_values, 8, 32))
        segments = _superblock(8, 32, [(struct.pack(">Q", 51), None)], [segment_leaf])
    else:
        segments = _superblock(8, 32, segment_values, [])

    item = (struct.pack(">I", 3) + b"\0\3\0disk1/part1", b"\0\0\1" + struct.pack(">QQ", 41, 8))
    imap = _tree([item], 0, 0)
    point = bytearray(132)
    struct.pack_into(">I", point, 48, 2)
    slices = _tree([(struct.pack(">I", 3), point)], 4, 132)
    header = bytearray(1024)
    header[:4] = b"ARCH"
    struct.pack_into(">H", header, 8, 8)
    header[32:48] = bytes(range(16))
    for section in (imap, dmap, segments, b"", b"", slices):
        header.extend(_section(section))
    struct.pack_into(">I", header, 4, len(header))
    assert len(header) <= 4088
    arch = _page(1, header)
    commit = _page(2, b"ARCI")
    struct.pack_into(">H", commit, 12, 8)
    struct.pack_into(">IIQ", commit, 16, 2, 4096, 3 * 4096)
    struct.pack_into(">Q", commit, 32, 1)
    commit[136:152] = bytes(range(16))
    struct.pack_into(">Q", commit, 72, _commit_crc(commit))
    struct.pack_into(">I", commit, 4, 0)
    struct.pack_into(">I", commit, 4, _crc32c(commit))
    return bytes(arch + _page(1, b"ARCH") + commit + arch + b"".join(pages))


def _shared_extent_archive():
    def extent(volume, length, index):
        key = struct.pack(">QQ", volume, 0) + length.to_bytes(3, "big") + struct.pack(">IQ", 3, 1)
        return key, struct.pack(">QH", 51, index)

    dmap = _superblock(31, 10, [extent(41, 4, 0), extent(42, 4, 4)], [])
    segments = _superblock(8, 32, [(struct.pack(">Q", 51), struct.pack(">Q", (1 << 56) | 4) + bytes(24))], [])
    items = [(struct.pack(">I", 3) + b"\0\3\0disk1/" + name,
              b"\0\0\1" + struct.pack(">QQ", volume, 4))
             for name, volume in ((b"part1", 41), (b"part2", 42))]
    point = bytearray(132)
    struct.pack_into(">I", point, 48, 2)
    slices = _tree([(struct.pack(">I", 3), point)], 4, 132)
    header = bytearray(1024)
    header[:4] = b"ARCH"
    struct.pack_into(">H", header, 8, 8)
    header[32:48] = bytes(range(16))
    for section in (_tree(items, 0, 0), dmap, segments, b"", b"", slices):
        header.extend(_section(section))
    struct.pack_into(">I", header, 4, len(header))
    arch = _page(1, header)
    commit = _page(2, b"ARCI")
    struct.pack_into(">H", commit, 12, 8)
    struct.pack_into(">IIQ", commit, 16, 2, 4096, 3 * 4096)
    struct.pack_into(">Q", commit, 32, 1)
    commit[136:152] = bytes(range(16))
    struct.pack_into(">Q", commit, 72, _commit_crc(commit))
    struct.pack_into(">I", commit, 4, 0)
    struct.pack_into(">I", commit, 4, _crc32c(commit))
    return bytes(arch + _page(1, b"ARCH") + commit + arch + _segment(b"AAAA" + bytes(60) + b"BBBB"))


def test_shared_segment_sparse_indices_across_volumes(peare_lib, tmp_path):
    path = tmp_path / "shared-segment.tibx"
    path.write_bytes(_shared_extent_archive())
    root = peare_lib.open_file(str(path))
    assert root is not None
    directory = None
    try:
        source = _directory_source(root, "disk1")
        try:
            status, directory = _open_source_with_password(peare_lib, source, None)
        finally:
            peare_lib._lib.peare_source_destroy(source)
        assert status == 0 and directory is not None
        find = directory._L.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        for name, expected in ((b"part1", b"AAAA"), (b"part2", b"BBBB")):
            resource = _HANDLE()
            assert find(directory._h, b"TIBX_VOLUME", name, None, C.byref(resource)) == 0
            try:
                assert peare_lib.read_resource(resource, 0, 4) == expected
                assert peare_lib.read_resource(resource, 1, 2) == expected[1:3]
            finally:
                peare_lib._lib.peare_resource_destroy(resource)
    finally:
        if directory:
            directory.close()
        root.close()


@pytest.mark.parametrize("case,expected", [
    ("baseline", b"AAAABBBB"),
    ("inline_tombstone", bytes(4) + b"BBBB"),
    ("inline_replacement", b"CCCCBBBB"),
    ("inline_last_tombstone", bytes(4) + b"BBBB"),
    ("inline_last_replacement", b"CCCCBBBB"),
    ("level_tombstone", bytes(4) + b"BBBB"),
    ("missing_segment", None),
    ("physical_segment_missing", None),
    ("zlib_valid", b"AAAABBBB"),
    ("zlib_bad_adler", None),
    ("zlib_truncated", None),
    ("zlib_raw_deflate", None),
    ("zlib_wrong_size", None),
])
def test_synthetic_tibx_datamap_retention_abi(peare_lib, tmp_path, case, expected):
    path = tmp_path / "synthetic-datamap-retention.tibx"
    path.write_bytes(_archive(case))
    root = peare_lib.open_file(str(path))
    assert root is not None
    directory, resource = None, _HANDLE()
    try:
        source = _directory_source(root, "disk1")
        try:
            status, directory = _open_source_with_password(peare_lib, source, None)
        finally:
            peare_lib._lib.peare_source_destroy(source)
        assert status == 0 and directory is not None
        find = directory._L.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        assert find(directory._h, b"TIBX_VOLUME", b"part1", None, C.byref(resource)) == 0
        buffer = (C.c_uint8 * 8)()
        count = C.c_size_t()
        status = directory._L.peare_resource_read(resource, 0, buffer, 8, C.byref(count))
        if expected is None:
            assert status != 0 and count.value == 0
        else:
            assert status == 0 and bytes(buffer[:count.value]) == expected
            assert peare_lib.read_resource(resource, 2, 4) == expected[2:6]
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if directory is not None:
            directory.close()
        root.close()


def test_synthetic_tibx_segment_callback_error_is_not_zero_data(peare_lib, tmp_path):
    path = tmp_path / "synthetic-datamap-io.tibx"
    path.write_bytes(_archive("baseline"))
    source = CallbackSource(peare_lib, path)
    root = directory = None
    resource, nested = _HANDLE(), _HANDLE()
    try:
        status, root = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and root is not None
        nested = _directory_source(root, "disk1")
        status, directory = _open_source_with_password(peare_lib, nested, None)
        assert status == 0 and directory is not None
        find = directory._L.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        assert find(directory._h, b"TIBX_VOLUME", b"part1", None, C.byref(resource)) == 0
        source.mode = "error"
        before = source.calls
        buffer = (C.c_uint8 * 8)(*([0xa5] * 8))
        count = C.c_size_t()
        status = directory._L.peare_resource_read(resource, 0, buffer, 8, C.byref(count))
        assert source.calls > before
        assert status != 0 and count.value == 0
        assert bytes(buffer) == bytes([0xa5] * 8)
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if directory is not None:
            directory.close()
        if nested:
            peare_lib._lib.peare_source_destroy(nested)
        if root is not None:
            root.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


def test_synthetic_tibx_second_segment_error_is_atomic(peare_lib, tmp_path):
    class FailingSecondSegment(CallbackSource):
        def __init__(self, lib, path):
            self.fail = False
            self.failures = 0
            super().__init__(lib, path)

        def read(self, context, offset, buffer, capacity, out_read):
            if self.fail and offset <= 5 * 4096 < offset + capacity:
                self.failures += 1
                return 13
            return super().read(context, offset, buffer, capacity, out_read)

    path = tmp_path / "synthetic-datamap-second-io.tibx"
    path.write_bytes(_archive("baseline"))
    source = FailingSecondSegment(peare_lib, path)
    root = directory = None
    resource, nested = _HANDLE(), _HANDLE()
    try:
        status, root = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and root is not None
        nested = _directory_source(root, "disk1")
        status, directory = _open_source_with_password(peare_lib, nested, None)
        assert status == 0 and directory is not None
        find = directory._L.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        assert find(directory._h, b"TIBX_VOLUME", b"part1", None, C.byref(resource)) == 0
        source.fail = True
        buffer = (C.c_uint8 * 8)(*([0xa5] * 8))
        count = C.c_size_t()
        status = directory._L.peare_resource_read(resource, 0, buffer, 8, C.byref(count))
        assert source.failures > 0
        assert status != 0 and count.value == 0
        assert bytes(buffer) == bytes([0xa5] * 8)
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if directory is not None:
            directory.close()
        if nested:
            peare_lib._lib.peare_source_destroy(nested)
        if root is not None:
            root.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1
