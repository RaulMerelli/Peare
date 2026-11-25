"""Synthetic, small NTFS/WOF/WIM ABI vectors; not an authentic filesystem."""

import ctypes as C
import hashlib
import struct
from concurrent.futures import ThreadPoolExecutor

import pytest

from peare_opener import _HANDLE


PAYLOAD = b"Synthetic WOF payload for Peare.\n"
PAYLOAD_SHA256 = "b0db10feb95d7dd67811029549e9e2b159e14b28d9051bb16c590703ebb84be7"
GUID = bytes(range(16))
SECTOR = 512
CLUSTER = 4096
RECORD = 1024


def _align(value, multiple=8):
    return (value + multiple - 1) // multiple * multiple


def _resident(kind, value, name="", attr_id=0):
    encoded = name.encode("utf-16le")
    offset = _align(24 + len(encoded))
    length = _align(offset + len(value))
    data = bytearray(length)
    struct.pack_into("<IIBBHHH", data, 0, kind, length, 0, len(encoded) // 2,
                     24 if encoded else 0, 0, attr_id)
    struct.pack_into("<IH", data, 16, len(value), offset)
    data[24:24 + len(encoded)] = encoded
    data[offset:offset + len(value)] = value
    return data


def _nonresident(kind, lcn, clusters, size, attr_id=0, name=""):
    assert 0 < lcn < 128 and 0 < clusters < 128
    encoded = name.encode("utf-16le")
    runs_offset = _align(64 + len(encoded))
    data = bytearray(_align(runs_offset + 4))
    struct.pack_into("<IIBBHHH", data, 0, kind, len(data), 1, len(encoded) // 2,
                     64 if encoded else 0, 0, attr_id)
    struct.pack_into("<QQH", data, 16, 0, clusters - 1, runs_offset)
    struct.pack_into("<QQQ", data, 40, clusters * CLUSTER, size, size)
    data[64:64 + len(encoded)] = encoded
    data[runs_offset:runs_offset + 4] = bytes((0x11, clusters, lcn, 0))
    return data


def _record(number, attributes, directory=False, base_reference=0, record_size=RECORD):
    data = bytearray(record_size)
    data[:4] = b"FILE"
    usa_count = record_size // SECTOR + 1
    struct.pack_into("<HH", data, 4, 48, usa_count)
    struct.pack_into("<H", data, 16, 1)
    first_attribute = _align(48 + usa_count * 2)
    struct.pack_into("<H", data, 20, first_attribute)
    struct.pack_into("<H", data, 22, 3 if directory else 1)
    struct.pack_into("<Q", data, 32, base_reference)
    pos = first_attribute
    for attr in attributes:
        data[pos:pos + len(attr)] = attr
        pos += len(attr)
    assert pos + 4 < record_size
    struct.pack_into("<I", data, pos, 0xffffffff)
    struct.pack_into("<II", data, 24, pos + 4, record_size)
    struct.pack_into("<I", data, 44, number)
    marker = b"\xa5\x5a"
    data[48:50] = marker
    for i in range(usa_count - 1):
        tail = (i + 1) * SECTOR - 2
        data[50 + i * 2:52 + i * 2] = data[tail:tail + 2]
        data[tail:tail + 2] = marker
    return data


def _index_entry(name, record, size=0, directory=False):
    text = name.encode("utf-16le")
    key = bytearray(66 + len(text))
    struct.pack_into("<Q", key, 48, size)
    struct.pack_into("<I", key, 56, 0x10000000 if directory else 0)
    key[64] = len(text) // 2
    key[65] = 1
    key[66:] = text
    length = _align(16 + len(key))
    entry = bytearray(length)
    struct.pack_into("<QHHH", entry, 0, (1 << 48) | record, length, len(key), 0)
    entry[16:16 + len(key)] = key
    return entry


def _index_root(entries):
    data = bytearray(32)
    struct.pack_into("<III", data, 0, 0x30, 1, CLUSTER)
    struct.pack_into("<I", data, 16, 16)
    for entry in entries:
        data.extend(entry)
    end = bytearray(16)
    struct.pack_into("<HHH", end, 8, 16, 0, 2)
    data.extend(end)
    struct.pack_into("<II", data, 20, len(data) - 16, len(data) - 16)
    return data


def _wim():
    content_hash = hashlib.sha1(PAYLOAD).digest()
    table = bytearray(50)
    struct.pack_into("<QQQH", table, 0, len(PAYLOAD), 258, len(PAYLOAD), 1)
    table[30:50] = content_hash
    header = bytearray(208)
    header[:8] = b"MSWIM\0\0\0"
    struct.pack_into("<I", header, 8, len(header))
    header[24:40] = GUID
    struct.pack_into("<HHI", header, 40, 1, 1, 1)
    struct.pack_into("<QQQ", header, 48, len(table), len(header), len(table))
    return bytes(header + table + PAYLOAD), content_hash, hashlib.sha1(table).digest()


def _catalog(backing="backing.wim", target=None):
    path = ("\\" + backing + "\0").encode("utf-16le")
    location_size = 104 + len(path)
    header = struct.pack("<IIIIQ", 0x66436f57, 1, 40, 1, 0)
    item = struct.pack("<QIIII16s", 0, 64, location_size, 0, 1, GUID)
    location = bytearray(104)
    for offset, value in ((8, location_size), (16, 5), (20, 1),
                          (24, location_size - 20), (28, 5), (32, 6), (40, 0x48)):
        struct.pack_into("<I", location, offset, value)
    if target is not None:
        disk_identity, partition_identity = target
        if isinstance(disk_identity, int):
            struct.pack_into("<Q", location, 48, partition_identity)
            struct.pack_into("<I", location, 68, 1)
            struct.pack_into("<I", location, 72, disk_identity)
        else:
            location[48:64] = partition_identity
            location[72:88] = disk_identity
    return header + item + location + path


def _attribute_list_index(attr_id, kind=0x90, record=11, sequence=1):
    name = "$I30".encode("utf-16le")
    entry = bytearray(_align(26 + len(name)))
    struct.pack_into("<IHBBQQH", entry, 0, kind, len(entry), len(name) // 2,
                     26, 0, (sequence << 48) | record, attr_id)
    entry[26:26 + len(name)] = name
    return entry


def _external_index(case):
    root = bytearray(32 + 24)
    struct.pack_into("<III", root, 0, 0x30, 1, CLUSTER)
    struct.pack_into("<III", root, 16, 16, 40, 40)
    root[28] = 1
    struct.pack_into("<HHH", root, 32 + 8, 24, 0, 3)
    struct.pack_into("<Q", root, 32 + 16, 0)

    entry = _index_entry("WimOverlay.dat", 12)
    end = bytearray(16)
    struct.pack_into("<HHH", end, 8, 16, 0, 2)
    block = bytearray(CLUSTER)
    block[:4] = b"INDX"
    struct.pack_into("<HHQ", block, 4, 40, CLUSTER // SECTOR + 1, 0)
    struct.pack_into("<Q", block, 16, 1 if case == "external_bad_vcn" else 0)
    struct.pack_into("<III", block, 24, 40, 40 + len(entry) + len(end), CLUSTER - 24)
    block[64:64 + len(entry)] = entry
    block[64 + len(entry):64 + len(entry) + len(end)] = end
    if case == "external_truncated_node":
        struct.pack_into("<H", block, 64 + 8, CLUSTER)
    marker = b"\xa6\x5b"
    block[40:42] = marker
    for i in range(CLUSTER // SECTOR):
        tail = (i + 1) * SECTOR - 2
        block[42 + i * 2:44 + i * 2] = block[tail:tail + 2]
        block[tail:tail + 2] = marker
    if case == "external_bad_fixup":
        block[-2:] = b"\0\0"
    return root, block


def _synthetic_volume(index_case="normal", target=None, missing_backing=False, sector_size=SECTOR):
    assert sector_size in (SECTOR, CLUSTER)
    assert sector_size == SECTOR or not index_case.startswith("external_")
    record_size = max(RECORD, sector_size)
    mft_clusters = 16 * record_size // CLUSTER
    upcase_lcn = 4 + mft_clusters
    wim, content_hash, table_hash = _wim()
    reparse = bytearray(96)
    struct.pack_into("<IHHIIIIQ", reparse, 0, 0x80000017, 88, 0, 1, 1, 2, 0, 0)
    reparse[32:52] = content_hash
    reparse[52:72] = table_hash
    struct.pack_into("<QQQ", reparse, 72, len(PAYLOAD), len(PAYLOAD), 258)
    unicode_case = index_case.startswith("upcase_")
    backing = "b\u00e4cking.wim" if unicode_case else "backing.wim"
    catalog = _catalog(backing, target)
    root_entries = [
        _index_entry("System Volume Information", 11, directory=True),
        _index_entry("payload.bin", 14, len(PAYLOAD)),
    ]
    if not missing_backing:
        root_entries.append(_index_entry("B\u00c4CKING.WIM" if unicode_case else backing, 13, len(wim)))
    if index_case in ("upcase_ambiguous", "upcase_same_ref"):
        root_entries.append(_index_entry(backing, 12 if index_case == "upcase_ambiguous" else 13))
    root = _index_root(root_entries)
    external = index_case.startswith("external_")
    block = None
    if external:
        system, block = _external_index(index_case)
    else:
        system = _index_root([_index_entry("WimOverlay.dat", 12, len(catalog))])
    if index_case == "truncated_index":
        # Entry length exceeds the resident index node, but the attribute itself fits.
        struct.pack_into("<H", system, 32 + 8, len(system))
    system_attrs = [_resident(0x90, system, "$I30")]
    if index_case in ("matching_list", "mismatching_list"):
        index_id = 0 if index_case == "matching_list" else 7
        system_attrs.append(_resident(0x20, _attribute_list_index(index_id), attr_id=1))
    if external:
        sequence = 2 if index_case == "external_bad_sequence" else 1
        entries = (_attribute_list_index(0) +
                   _attribute_list_index(2, 0xa0, 15, sequence) +
                   _attribute_list_index(3, 0xb0, 15))
        system_attrs.append(_resident(0x20, entries, attr_id=1))
    mft = {
        0: _record(0, [_nonresident(0x80, 4, mft_clusters, 16 * record_size)], record_size=record_size),
        5: _record(5, [_resident(0x90, root, "$I30")], True, record_size=record_size),
        10: _record(10, [_nonresident(0x80, upcase_lcn, 32,
                                      131070 if index_case == "upcase_truncated" else 131072)], record_size=record_size),
        11: _record(11, system_attrs, True, record_size=record_size),
        12: _record(12, [_resident(0x80, catalog)], record_size=record_size),
        13: _record(13, [_resident(0x80, wim)], record_size=record_size),
        14: _record(14, [_resident(0x80, bytes(len(PAYLOAD))), _resident(0xc0, reparse, attr_id=1)], record_size=record_size),
    }
    if external:
        base = (1 << 48) | (12 if index_case == "external_bad_base" else 11)
        mft[15] = _record(15, [_nonresident(0xa0, 40, 1, CLUSTER, 2, "$I30"),
                               _resident(0xb0, b"\x01", "$I30", 3)],
                          base_reference=base)
    image = bytearray(256 * 1024)
    image[3:11] = b"NTFS    "
    struct.pack_into("<HB", image, 11, sector_size, CLUSTER // sector_size)
    struct.pack_into("<QQ", image, 40, len(image) // sector_size, 4)
    struct.pack_into("<b", image, 64, -10 if record_size == RECORD else -12)
    image[510:512] = b"\x55\xaa"
    for number, data in mft.items():
        offset = 4 * CLUSTER + number * record_size
        image[offset:offset + record_size] = data
    upcase = bytearray(131072)
    for i in range(65536):
        mapped = i - 32 if 97 <= i <= 122 else i
        if i == 0x00e4 and index_case != "upcase_unmapped":
            mapped = 0x00c4
        struct.pack_into("<H", upcase, i * 2, mapped)
    image[upcase_lcn * CLUSTER:upcase_lcn * CLUSTER + len(upcase)] = upcase
    if block is not None:
        image[40 * CLUSTER:41 * CLUSTER] = block
    return bytes(image)


@pytest.mark.parametrize("index_case", [
    "normal", "matching_list", "truncated_index", "mismatching_list", "io_error",
    "external_valid", "external_bad_sequence", "external_bad_base", "external_bad_vcn",
    "external_truncated_node", "external_bad_fixup", "external_io_error",
    "upcase_valid", "upcase_same_ref", "upcase_ambiguous", "upcase_unmapped",
    "upcase_truncated", "upcase_io_error",
])
def test_synthetic_wof_ntfs_lookup_integrity_abi(peare_lib, index_case):
    image = _synthetic_volume(index_case)
    read_type = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.POINTER(C.c_uint8),
                            C.c_size_t, C.POINTER(C.c_size_t))
    release_type = C.CFUNCTYPE(None, C.c_void_p)
    state = {"armed": False, "failed": 0, "released": 0}
    record_start = 4 * CLUSTER + 11 * RECORD

    @read_type
    def read(_context, offset, buffer, capacity, out_read):
        out_read[0] = 0
        if index_case == "external_io_error":
            fail_start, fail_size = 40 * CLUSTER, CLUSTER
        elif index_case == "upcase_io_error":
            fail_start, fail_size = 8 * CLUSTER, 32 * CLUSTER
        else:
            fail_start, fail_size = record_start, RECORD
        if (state["armed"] and index_case in ("io_error", "external_io_error", "upcase_io_error") and
                offset < fail_start + fail_size and fail_start < offset + capacity):
            state["failed"] += 1
            return 13
        data = image[offset:offset + capacity]
        if data:
            C.memmove(buffer, data, len(data))
        out_read[0] = len(data)
        return 0

    @release_type
    def release(_context):
        state["released"] += 1

    lib = peare_lib._lib
    create = lib.peare_source_create_callback
    create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, read_type, release_type,
                       C.POINTER(_HANDLE)]
    create.restype = C.c_int
    source, resource = _HANDLE(), _HANDLE()
    assert create(len(image), b"synthetic-ntfs.img", None, read, release, C.byref(source)) == 0
    opener = None
    try:
        opener = peare_lib._open_source(source)
        assert opener is not None
        state["armed"] = True
        find = lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        status = find(opener._h, b"NTFS_FILE", b"payload.bin", None, C.byref(resource))
        if index_case in ("normal", "matching_list", "external_valid", "upcase_valid", "upcase_same_ref"):
            assert status == 0 and resource
            result = peare_lib.read_resource(resource, 0, len(PAYLOAD))
            assert result == PAYLOAD
            assert hashlib.sha256(result).hexdigest() == PAYLOAD_SHA256
        else:
            if status == 0:
                assert resource
                buffer = (C.c_uint8 * len(PAYLOAD))()
                count = C.c_size_t()
                status = lib.peare_resource_read(resource, 0, buffer, len(buffer), C.byref(count))
                assert count.value == 0
            assert status != 0
            if index_case in ("io_error", "external_io_error", "upcase_io_error"):
                assert state["failed"] > 0
    finally:
        if resource:
            lib.peare_resource_destroy(resource)
        if opener is not None:
            opener.close()
        lib.peare_source_destroy(source)
    assert state["released"] == 1


@pytest.mark.parametrize("sector_size", [SECTOR, CLUSTER])
def test_synthetic_ntfs_wof_wim_same_volume_abi(peare_lib, tmp_path, sector_size):
    path = tmp_path / "synthetic-ntfs-wof.img"
    path.write_bytes(_synthetic_volume(sector_size=sector_size))
    opener = peare_lib.open_file(str(path))
    assert opener is not None
    resource = _HANDLE()
    try:
        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        assert find(opener._h, b"NTFS_FILE", b"payload.bin", None, C.byref(resource)) == 0
        result = peare_lib.read_resource(resource, 0, len(PAYLOAD))
        assert result == PAYLOAD
        assert hashlib.sha256(result).hexdigest() == PAYLOAD_SHA256
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        opener.close()


def test_synthetic_wof_file_concurrent_reads(peare_lib, tmp_path):
    image = bytearray(_synthetic_volume())
    content = b"A" * CLUSTER + b"B" * CLUSTER
    compressed = struct.pack("<I", CLUSTER) + content
    reparse = struct.pack("<IHHIIII", 0x80000017, 16, 0, 1, 2, 1, 0)
    image[48 * CLUSTER:48 * CLUSTER + len(compressed)] = compressed
    record = _record(14, [_nonresident(0x80, 45, 2, len(content)),
                          _nonresident(0x80, 48, 3, len(compressed), 1, "WofCompressedData"),
                          _resident(0xc0, reparse, attr_id=2)])
    start = 4 * CLUSTER + 14 * RECORD
    image[start:start + RECORD] = record
    path = tmp_path / "synthetic-wof-concurrent.img"
    path.write_bytes(image)
    opener = peare_lib.open_file(str(path))
    assert opener is not None
    resource = _HANDLE()
    try:
        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        assert find(opener._h, b"NTFS_FILE", b"payload.bin", None, C.byref(resource)) == 0

        def read_worker(worker):
            for i in range(400):
                offset = (0, CLUSTER, CLUSTER - 32)[(i + worker) % 3]
                assert peare_lib.read_resource(resource, offset, 64) == content[offset:offset + 64]

        with ThreadPoolExecutor(max_workers=8) as executor:
            list(executor.map(read_worker, range(8)))
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        opener.close()
