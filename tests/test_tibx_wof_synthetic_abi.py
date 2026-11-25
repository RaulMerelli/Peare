"""Synthetic TIBX/GPT/NTFS/WOF vectors; not authentic backup data."""

import ctypes as C
import hashlib
import struct
import zlib

import pytest

from peare_opener import _HANDLE
from test_ntfs_wof_synthetic_abi import PAYLOAD, PAYLOAD_SHA256, _synthetic_volume
from test_tibx_abi import _directory_source, _open_source_with_password
from test_tibx_synthetic_abi import _commit_crc, _crc32c, _page, _section, _tree


DISK_GUID = bytes.fromhex("00112233445566778899aabbccddeeff")
MBR_SIGNATURE = 0x12345678
PART_GUIDS = [bytes.fromhex(x) for x in (
    "10112233445566778899aabbccddeeff",
    "20112233445566778899aabbccddeeff",
    "30112233445566778899aabbccddeeff",
)]


def _tag(tag, value):
    length = len(value)
    assert length <= 32767
    encoded = bytes([length]) if length < 128 else bytes([0x80 | (length >> 8), length & 255])
    return struct.pack("<H", tag) + encoded + value


def _number(value, width):
    return value.to_bytes(width, "little").rstrip(b"\0")


def _fields(values):
    return b"".join(_tag(tag, values[tag]) for tag in sorted(values))


def _disk(corrupt_crc=False, hybrid=False):
    data = bytearray(34 * 512)
    data[510:512] = b"\x55\xaa"
    data[446 + 4] = 0xee
    struct.pack_into("<II", data, 446 + 8, 1, 2000)
    if hybrid:
        data[446 + 16 + 4] = 7
        struct.pack_into("<II", data, 446 + 16 + 8, 64, 512)
    table = bytearray(4 * 128)
    for i, guid in enumerate(PART_GUIDS):
        offset = i * 128
        table[offset:offset + 16] = bytes.fromhex("aabbccddeeff00112233445566778899")
        table[offset + 16:offset + 32] = guid
        start = 64 + i * 512
        struct.pack_into("<QQ", table, offset + 32, start, start + 511)
    data[1024:1536] = table
    header = bytearray(512)
    header[:8] = b"EFI PART"
    struct.pack_into("<III", header, 8, 0x10000, 92, 0)
    struct.pack_into("<QQQQ", header, 24, 1, 2000, 34, 1900)
    header[56:72] = DISK_GUID
    struct.pack_into("<QIII", header, 72, 2, 4, 128, zlib.crc32(table))
    struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
    data[512:1024] = header
    if corrupt_crc:
        data[1024] ^= 1
    return bytes(data)


def _mbr_disk(case):
    extended = case.startswith("mbr_ebr")
    four_kn = case.startswith("mbr_4kn")
    size = (578 if case == "mbr_ebr_loop" else
            577 if extended and case != "mbr_ebr_truncated" else 16 if four_kn else
            1 if case in ("mbr_one_sector", "mbr_163_one_sector") else 4) * 512
    data = bytearray(size)
    struct.pack_into("<I", data, 440, MBR_SIGNATURE)
    data[510:512] = b"\x55\xaa"
    data[446 + 4] = 7
    struct.pack_into("<II", data, 446 + 8, 8 if four_kn else 64, 64 if four_kn else 512)
    second = 446 + 16
    data[second + 4] = 0x0f if extended else 7
    struct.pack_into("<II", data, second + 8, 72 if four_kn else 576,
                     64 if four_kn else 1024 if extended else 512)
    if case == "mbr_mixed":
        data[446 + 2 * 16 + 4] = 0xee
        struct.pack_into("<II", data, 446 + 2 * 16 + 8, 1, 2000)
    if case == "mbr_163_other":
        data[446 + 2 * 16 + 4] = 7
        struct.pack_into("<II", data, 446 + 2 * 16 + 8, 1088, 512)
    if case == "mbr_gpt_header":
        data[512:520] = b"EFI PART"
    if case == "mbr_gpt_valid_residue":
        residual = bytearray(_disk())
        residual[:512] = data[:512]
        data = residual
    if extended and len(data) >= 577 * 512:
        offset = 576 * 512
        data[offset + 510:offset + 512] = b"\x55\xaa"
        data[offset + 446 + 4] = 7
        struct.pack_into("<II", data, offset + 446 + 8, 1, 512)
        if case == "mbr_ebr_bad_signature":
            data[offset + 510:offset + 512] = b"\0\0"
        if case == "mbr_ebr_loop":
            data[offset + 446 + 16 + 4] = 0x0f
            struct.pack_into("<II", data, offset + 446 + 16 + 8, 1, 1023)
            second_ebr = 577 * 512
            data[second_ebr + 510:second_ebr + 512] = b"\x55\xaa"
            data[second_ebr + 446 + 4] = 7
            struct.pack_into("<II", data, second_ebr + 446 + 8, 1, 1)
            data[second_ebr + 446 + 16 + 4] = 0x0f
            struct.pack_into("<II", data, second_ebr + 446 + 16 + 8, 1, 1023)
    return bytes(data)


def _segment_pages(data):
    packed = zlib.compress(data)
    payload = bytearray(36)
    payload[:4] = b"SG\0\1"
    struct.pack_into(">II", payload, 4, len(data), len(packed))
    payload[16] = 2
    chunks = [packed[:4052]]
    packed = packed[4052:]
    while packed:
        chunks.append(packed[:4088])
        packed = packed[4088:]
    return [_page(255, payload + chunks[0])] + [_page(255, chunk) for chunk in chunks[1:]]


def test_synthetic_mbr_residual_gpt_crc_and_single_sector():
    single = _mbr_disk("mbr_one_sector")
    assert len(single) == 512 and single[510:512] == b"\x55\xaa"
    assert all(single[446 + 16 * i + 4] != 0xee for i in range(4))
    disk = _mbr_disk("mbr_gpt_valid_residue")
    assert all(disk[446 + 16 * i + 4] != 0xee for i in range(4))
    header = bytearray(disk[512:1024])
    assert header[:8] == b"EFI PART"
    size, expected = struct.unpack_from("<II", header, 12)
    struct.pack_into("<I", header, 16, 0)
    assert zlib.crc32(header[:size]) == expected
    lba = struct.unpack_from("<Q", header, 72)[0]
    count, width, table_crc = struct.unpack_from("<III", header, 80)
    table = disk[lba * 512:lba * 512 + count * width]
    assert len(table) == count * width and zlib.crc32(table) == table_crc


def _archive(case, real=None):
    mbr = case.startswith("mbr_")
    four_kn = case.startswith("mbr_4kn")
    sector_size = 4096 if four_kn else 512
    mbr_start = (real["target_start"] if mbr and real is not None else
                 72 if four_kn else 577 if case.startswith("mbr_ebr") else 576)
    mbr_signature = real["mbr_signature"] if mbr and real is not None else MBR_SIGNATURE
    disk_guid = DISK_GUID if real is None or mbr else real["disk_guid"]
    part_guids = PART_GUIDS if real is None or mbr else real["part_guids"]
    disk_fields = {2: _number(sector_size, 4), 18: _number(2001, 8), 74: b"\1", 32774: disk_guid}
    if mbr:
        del disk_fields[32774]
        disk_fields[32773] = _number(mbr_signature + (case == "mbr_bad_signature"), 4)
    if case.startswith("getter_"):
        disk_fields.update({73: b"\1", 83: b"", 129: b"\2AB", 32785: b""})
    disk_metadata = real["disk_metadata"] if real is not None and (
        case in ("real_disk", "real_gpt_valid", "real_gpt_corrupt") or case.startswith("mbr_163_real_")) else _fields(disk_fields)
    disk_data = _mbr_disk(case) if mbr else _disk(case == "bad_gpt_crc", case == "gpt_hybrid")
    streams = [(10, disk_data if real is None else real["disk"]), (11, disk_metadata)]
    sizes = {}
    items = []

    def item(name, primary_id, secondary_id, volume):
        descriptors = (bytes((0, 1)) + struct.pack(">QQ", primary_id, sizes.get(primary_id, len(dict(streams)[primary_id]))) +
                       bytes((1, 1)) + struct.pack(">QQ", secondary_id, len(dict(streams)[secondary_id])))
        return struct.pack(">I", 3) + (b"\0\3\0" if volume else b"\0\1\0") + name, b"\1" + descriptors

    items.append(item(b"disk1", 10, 11, False))
    count = 3 if case in ("indeterminate", "mbr_ambiguous", "mbr_163_other", "mbr_163_real_residue", "mbr_163_real_one_sector") or case.startswith(("type4_", "filesystem_")) else 2
    for i in range(count):
        target_offset = mbr_start if case in ("mbr_catalog_lba", "mbr_4kn_catalog_lba") else mbr_start * sector_size
        target = (mbr_signature, target_offset) if mbr else (disk_guid, part_guids[1])
        image = _synthetic_volume(target=target if i == 0 else None,
                                  missing_backing=(i == 0), sector_size=sector_size)
        if real is not None and i in real.get("partition_prefixes", {}):
            prefix = real["partition_prefixes"][i]
            assert len(prefix) <= len(image)
            image = prefix + bytes(len(image) - len(prefix))
        if i == 2 and (case.startswith(("type4_", "filesystem_")) or case == "mbr_163_other"):
            image = bytes(len(image))
        if i == 1 and case == "bad_wim_hash":
            image = bytearray(image)
            pos = image.find(PAYLOAD)
            assert pos >= 0
            image[pos] ^= 1
            image = bytes(image)
        fields = {2: _number(sector_size, 4), 13: b"", 17: _number(8 if four_kn else 64 + i * 512, 8),
                  18: _number(64 if four_kn else 512, 8), 102: b"", 106: part_guids[i], 165: b"", 32803: b""}
        if mbr:
            del fields[106]
            if i:
                start = 1088 if i == 2 and case == "mbr_163_other" else mbr_start + (case == "mbr_bad_geometry")
                fields[17] = _number(start, 8)
        if case.startswith("mbr_163_"):
            del fields[165]
            fields[163] = b""
            fields[32802] = b"\xff\0\x80"
            if case == "mbr_163_empty":
                fields.update({tag: b"" for tag in (*range(61, 69), 69, 70, 71, 107)})
            elif case == "mbr_163_values":
                fields.update({61: b"\x7f", 62: b"\xff", 69: b"\xff\xff",
                               70: b"\xff\xff\xff\xff", 107: b"\x7f"})
            elif i == 1 and case == "mbr_163_long":
                fields[70] = bytes(5)
            elif i == 1 and case == "mbr_163_long_byte":
                fields[61] = bytes(2)
            elif i == 1 and case == "mbr_163_long_107":
                fields[107] = bytes(2)
            elif i == 1 and case == "mbr_163_bad_marker":
                fields[163] = b"\1"
            elif i == 1 and case == "mbr_163_precedence":
                fields[162] = b""
            elif i == 1 and case == "mbr_163_extra_165":
                fields[165] = b""
            elif i == 1 and case == "mbr_163_unsupported":
                fields[186] = b"\1"
            elif i == 1 and case == "mbr_163_exclusive_165":
                fields[182] = bytes(16)
            if i == 2 and case == "mbr_163_other":
                del fields[13], fields[102], fields[32803]
                fields.update({15: b"", 54: b"\1", 55: b"\1"})
        if case.startswith("getter_"):
            fields.update({8: b"", 19: _number(512, 8), 23: b"", 31: b"\1",
                           56: b"", 58: b"", 60: b"\1", 69: b"\1", 129: b"\2AB",
                           182: bytes(16), 203: b"abc"})
        if i == 1 and case == "getter_bad_width":
            fields[31] = bytes(5)
        if i == 1 and case == "getter_bad_flag":
            fields[23] = b"\1"
        if i == 1 and case == "getter_bad_text":
            fields[129] = b"\2A"
        if i == 1 and case == "getter_bad_precedence":
            fields[59] = b""
        if i == 1 and case.startswith("threshold_"):
            del fields[102], fields[32803]
            fields.update({3: b"\1", 32: _number(4097, 8), 47: b"\2", 52: b"\5"})
            if case == "threshold_equal":
                fields[52] = b"\4"
            elif case == "threshold_zero_product":
                fields[3] = _number(1 << 23, 4)
            elif case in ("threshold_zero_t32", "threshold_zero_t32_equal"):
                fields[32] = b""
                fields[52] = b"\2" if case == "threshold_zero_t32_equal" else b"\3"
        if i == 1 and case == "getter_102_precedence":
            fields[52] = b"\xff"
        if i == 1 and case == "getter_32803_oversize":
            fields[32803] = bytes(41)
        if i == 1 and case == "getter_32803_absent":
            del fields[32803]
        if i == 2 and case.startswith("type4_"):
            del fields[13], fields[102], fields[32803]
            fields.update({49: b"\1", 50: b"", 51: b"\1", 224: b"", 231: b"", 243: b""})
            if case == "type4_nonzero_flag":
                fields[243] = b"\1"
            elif case == "type4_long_49":
                fields[49] = bytes(9)
            elif case == "type4_long_marker":
                fields[231] = b"\1"
            elif case == "type4_long_51":
                fields[51] = bytes(5)
            elif case == "type4_long_flag":
                fields[243] = bytes(2)
            elif case == "type4_earlier_marker":
                fields[9] = b""
        if i == 2 and case.startswith("filesystem_"):
            del fields[13], fields[102], fields[32803]
            marker = {"filesystem_9_valid": 9, "filesystem_14_valid": 14,
                      "filesystem_15_valid": 15, "filesystem_206_valid": 206,
                      "filesystem_14_long_54": 14, "filesystem_15_long_55": 15,
                      "filesystem_206_bad_marker": 206, "filesystem_9_precedence": 9,
                      "filesystem_206_precedence": 206}[case]
            fields[marker] = b""
            if marker in (14, 15):
                fields[54], fields[55] = b"\1", b"\1"
            if case == "filesystem_14_long_54":
                fields[54] = bytes(33)
            elif case == "filesystem_15_long_55":
                fields[55] = bytes(2)
            elif case == "filesystem_206_bad_marker":
                fields[206] = b"\1"
            elif case == "filesystem_9_precedence":
                fields[206] = b""
            elif case == "filesystem_206_precedence":
                fields[251] = b""
        if i == 2 and case == "indeterminate":
            fields[201] = b"\1"  # Required field outside the supported decoder branch.
        if i == 1 and case in ("changes_empty", "changes_arbitrary", "changes_max", "changes_truncated"):
            fields[32787] = {"changes_empty": b"", "changes_arbitrary": b"\xff\0\x80\x01",
                             "changes_max": bytes(range(256)) * 127 + bytes(255),
                             "changes_truncated": b"abc"}[case]
            if case == "changes_truncated":
                del fields[32803]
        supplied_metadata = None if real is None else real["partition_metadata"][i]
        metadata = _fields(fields) if supplied_metadata is None else supplied_metadata
        if i == 1 and case == "changes_truncated":
            metadata = metadata[:-1]
        if real is not None and real["partition_sizes"][i] is not None:
            sizes[20 + i * 2] = real["partition_sizes"][i]
        streams.extend(((20 + i * 2, image), (21 + i * 2, metadata)))
        items.append(item(b"disk1/part" + str(i + 1).encode(), 20 + i * 2, 21 + i * 2, True))

    segment_pages = []
    extents, segments = [], []
    for stream_id, data in streams:
        segment_id = stream_id + 100
        pages = _segment_pages(data)
        first_page = 4 + len(segment_pages)
        assert len(pages) < 256
        segment_pages.extend(pages)
        key = struct.pack(">QQ", stream_id, 0) + len(data).to_bytes(3, "big") + struct.pack(">IQ", 3, 0)
        extents.append((key, struct.pack(">QH", segment_id, 0xffff)))
        segments.append((struct.pack(">Q", segment_id), struct.pack(">Q", (len(pages) << 56) | first_page) + bytes(24)))
    point = bytearray(132)
    struct.pack_into(">I", point, 48, 2)
    header = bytearray(1024)
    header[:4] = b"ARCH"
    struct.pack_into(">H", header, 8, 8)
    header[32:48] = bytes(range(16))
    for section in (_tree(items, 0, 0), _tree(extents, 31, 10), _tree(segments, 8, 32),
                    b"", b"", _tree([(struct.pack(">I", 3), point)], 4, 132)):
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
    return bytes(arch + _page(1, b"ARCH") + commit + arch + b"".join(segment_pages))


@pytest.mark.parametrize("case", ["valid", "getter_valid", "indeterminate", "bad_gpt_crc", "bad_wim_hash", "gpt_hybrid",
                                  "getter_bad_width", "getter_bad_flag", "getter_bad_text", "getter_bad_precedence",
                                  "getter_102_precedence", "getter_32803_oversize", "getter_32803_absent", "threshold_valid", "threshold_equal",
                                  "threshold_zero_product", "threshold_zero_t32", "threshold_zero_t32_equal",
                                  "type4_valid", "type4_nonzero_flag", "type4_long_49", "type4_long_51",
                                  "type4_long_flag", "type4_long_marker", "type4_earlier_marker",
                                  "filesystem_9_valid", "filesystem_14_valid", "filesystem_15_valid", "filesystem_206_valid",
                                  "filesystem_14_long_54", "filesystem_15_long_55", "filesystem_206_bad_marker",
                                  "filesystem_9_precedence", "filesystem_206_precedence",
                                  "changes_empty", "changes_arbitrary", "changes_max", "changes_truncated",
                                  "mbr_valid", "mbr_bad_signature", "mbr_bad_geometry", "mbr_ambiguous", "mbr_mixed",
                                  "mbr_gpt_header", "mbr_ebr_valid", "mbr_ebr_truncated", "mbr_ebr_bad_signature",
                                  "mbr_ebr_loop", "mbr_163_valid", "mbr_163_empty", "mbr_163_other",
                                  "mbr_163_long", "mbr_163_bad_marker", "mbr_163_precedence", "mbr_163_extra_165",
                                  "mbr_163_unsupported", "mbr_163_exclusive_165", "mbr_163_values",
                                  "mbr_163_long_byte", "mbr_163_long_107", "mbr_catalog_lba",
                                  "mbr_4kn_valid", "mbr_4kn_catalog_lba", "mbr_one_sector",
                                  "mbr_163_one_sector", "mbr_gpt_valid_residue"])
def test_synthetic_tibx_cross_volume_wof(peare_lib, tmp_path, case):
    _check_cross_volume(peare_lib, tmp_path, _archive(case),
                        case in ("valid", "getter_valid", "getter_32803_absent", "threshold_valid", "threshold_zero_t32",
                                 "type4_valid", "filesystem_9_valid", "filesystem_14_valid", "filesystem_15_valid",
                                 "filesystem_206_valid", "changes_empty", "changes_arbitrary", "changes_max",
                                 "mbr_valid", "mbr_ebr_valid", "mbr_163_valid", "mbr_163_empty", "mbr_163_other", "mbr_163_values",
                                 "mbr_4kn_valid", "mbr_one_sector", "mbr_163_one_sector", "mbr_gpt_header",
                                 "mbr_gpt_valid_residue"))


def _check_cross_volume(peare_lib, tmp_path, archive, expected_success):
    path = tmp_path / "synthetic-tibx-wof.tibx"
    path.write_bytes(archive)
    root = peare_lib.open_file(str(path))
    assert root is not None
    directory = ntfs = None
    resource, source = _HANDLE(), _HANDLE()
    try:
        source = _directory_source(root, "disk1")
        status, directory = _open_source_with_password(peare_lib, source, None)
        peare_lib._lib.peare_source_destroy(source)
        source = _HANDLE()
        assert status == 0 and directory is not None
        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        assert find(directory._h, b"TIBX_VOLUME", b"part1", None, C.byref(resource)) == 0
        assert peare_lib._lib.peare_resource_get_source(resource, C.byref(source)) == 0
        status, ntfs = _open_source_with_password(peare_lib, source, None)
        assert status == 0 and ntfs is not None
        peare_lib._lib.peare_resource_destroy(resource)
        resource = _HANDLE()
        status = find(ntfs._h, b"NTFS_FILE", b"payload.bin", None, C.byref(resource))
        if not expected_success:
            if status == 0:
                output = (C.c_uint8 * len(PAYLOAD))()
                count = C.c_size_t()
                status = peare_lib._lib.peare_resource_read(resource, 0, output, len(output), C.byref(count))
                assert count.value == 0
            assert status != 0
        else:
            assert status == 0 and resource
            data = peare_lib.read_resource(resource, 0, len(PAYLOAD))
            assert data == PAYLOAD and hashlib.sha256(data).hexdigest() == PAYLOAD_SHA256
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if source:
            peare_lib._lib.peare_source_destroy(source)
        if ntfs is not None:
            ntfs.close()
        if directory is not None:
            directory.close()
        root.close()
