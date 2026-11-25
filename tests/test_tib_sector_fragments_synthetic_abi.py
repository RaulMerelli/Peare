"""Synthetic SequentialChunkMap fragments, not authentic TIB samples."""

import ctypes as C
import struct
import zlib

import pytest

from peare_opener import _HANDLE

DEDUP_MODES = {
    "dedup_inline", "dedup_reference", "dedup_mixed", "dedup_many", "dedup_sparse",
    "dedup_many_refs", "dedup_small_sparse", "dedup_bad_ref", "dedup_bad_adler",
    "dedup_trailing", "raw_dedup", "raw_dedup_mixed", "raw_dedup_bad_checksum",
}
RAW_MODES = {"raw", "raw_bad_checksum", "raw_dedup", "raw_dedup_mixed", "raw_dedup_bad_checksum"}


def _tlv(tag, value, size=None):
    size = size or max(1, (value.bit_length() + 7) // 8)
    return struct.pack("<HB", tag, size) + value.to_bytes(size, "little")


def _map(records, cluster_sectors=1, duplicate=False, order=(2, 3, 4, 6), sector_width=2, extra_field=b"", dedup=False, clusters_per_block=1, raw=False, raw_marker=b"", duplicate_raw=False, external_chunks=False):
    values = {2: 512, 3: cluster_sectors, 4: clusters_per_block, 6: len(records)}
    header = b"".join(_tlv(tag, values[tag], sector_width if tag == 2 else None)
                      for tag in order)
    if duplicate:
        header += _tlv(6, len(records))
    if extra_field:
        header += struct.pack("<HB", 0x101, len(extra_field)) + extra_field
    if dedup:
        header += struct.pack("<HB", 0xd4, 0)
    if external_chunks:
        header += struct.pack("<HB", 0xf7, 0)
    if raw:
        header += struct.pack("<HB", 0x63, len(raw_marker)) + raw_marker
        if duplicate_raw:
            header += struct.pack("<HB", 0x63, 0)
    previous = 0
    rows = []
    for offset, size in records:
        if offset is None:
            rows.append(struct.pack("<QI", 1, size))
            continue
        delta = offset - previous
        encoded = (abs(delta) << 1) | (delta < 0)
        rows.append(struct.pack("<QI", encoded, size))
        previous = offset + size
    transposed = bytes(rows[row][col] for col in range(12) for row in range(len(rows)))
    encoded_map = (transposed + struct.pack("<I", zlib.adler32(transposed))
                   if raw else zlib.compress(transposed))
    return bytes([len(header)]) + header + encoded_map


def _archive(mode="fragments", order=(2, 3, 4, 6), sector_width=2, boundary=False, extra_field=b"", raw_marker=b"", duplicate_raw=False, reference_hash=bytes(16), boot_reference=None, reference_hashes=None):
    header = bytearray(32)
    struct.pack_into("<IHHQIII", header, 0, 0xa2b924ce, 32, 0, 1, 1, 1, 0)
    struct.pack_into("<I", header, 28, 32)
    struct.pack_into("<I", header, 24, zlib.adler32(header))
    raw = mode in RAW_MODES
    dedup = mode in DEDUP_MODES
    block_size = 65536 if mode in ("dedup_many", "dedup_many_refs") else 16384 if mode == "dedup_small_sparse" else 8192 if mode in ("dedup_mixed", "dedup_sparse", "raw_dedup_mixed") else 4096 if dedup else 512
    boot = bytearray(b"A" * block_size)
    boot[3:11] = b"NTFS    "
    struct.pack_into("<H", boot, 11, 512)
    boot[13] = 1 if mode == "dedup_small_sparse" else 8 if mode == "dedup_sparse" else block_size // 512
    struct.pack_into("<Q", boot, 40, block_size * 2 // 512 - 1)
    boot[510:512] = b"\x55\xaa"
    internal_bitmap = bytes((block_size // 4096 + 7) // 8) if dedup else b""
    first_payload = internal_bitmap + (
        b"".join(boot[i * 512:(i + 1) * 512] for i in range(0, 32, 2))
        if mode == "dedup_small_sparse" else boot)
    if boot_reference is not None:
        first_payload = b"\x01" + boot_reference
    if mode == "dedup_small_sparse":
        first_payload = b"\0" + first_payload[len(internal_bitmap):]
    first_stream = first_payload + struct.pack("<I", zlib.adler32(first_payload)) if raw else zlib.compress(first_payload)
    first_bitmap = b"\x55" * 4 if mode == "dedup_small_sparse" else b"\x03" if mode == "dedup_sparse" else b"\x01"
    first = first_bitmap + first_stream
    data = bytearray(header + first)
    if mode not in ("single", "large_stream", "corrupt_second") and not dedup and not raw:
        data += _map([(0, len(first))], order=order, sector_width=sector_width, extra_field=extra_field)
    if mode == "large_stream":
        encoder = zlib.compressobj(wbits=-15)
        deflate = encoder.compress(b"B" * 512) + encoder.flush()
        stream = b"\x78\x01" + b"\0\0\0\xff\xff" * 200 + deflate
        stream += struct.pack(">I", zlib.adler32(b"B" * 512))
        assert zlib.decompress(stream) == b"B" * 512
        second = b"\x01" + stream
    else:
        if mode == "dedup_reference":
            content = b"\x01" + reference_hash
        elif mode == "dedup_bad_ref":
            content = b"\x01" + bytes(15)
        elif mode in ("dedup_mixed", "raw_dedup_mixed", "dedup_small_sparse"):
            content = b"\x02" + b"B" * 4096 + reference_hash
        elif mode == "dedup_sparse":
            content = b"\0" + b"B" * 4096
        elif mode == "dedup_many_refs":
            content = b"\x01\x81" + b"".join(
                (reference_hashes[i] if reference_hashes and i in reference_hashes else
                 bytes([i]) * (16 if i in (0, 8, 15) else 4096)) for i in range(16))
        else:
            content = internal_bitmap + b"B" * block_size
        stream = content + struct.pack("<I", zlib.adler32(content)) if raw else zlib.compress(content)
        if mode == "dedup_trailing":
            stream += b"\0"
        second_bitmap = b"\x55" * 4 if mode == "dedup_small_sparse" else b"\x02" if mode == "dedup_sparse" else b"\x01"
        second = second_bitmap + stream
        if mode in ("corrupt_second", "raw_bad_checksum", "dedup_bad_adler", "raw_dedup_bad_checksum"):
            second = second[:-1] + bytes([second[-1] ^ 1])
    if boundary and mode != "single":
        target = 32 + 1024 * 1024 - 1
        data.extend(bytes(target - len(data) - len(second)))
    second_offset = len(data) - 32
    data += second
    records = [(second_offset, len(second))]
    if mode in ("overlap", "single", "large_stream", "corrupt_second") or dedup or raw:
        records.insert(0, (0, len(first)))
    data += _map(records, cluster_sectors=2 if mode == "geometry" else boot[13],
                 duplicate=mode == "duplicate", order=order, sector_width=sector_width, extra_field=extra_field,
                 dedup=dedup, clusters_per_block=32 if mode == "dedup_small_sparse" else 2 if mode == "dedup_sparse" else 1, raw=raw, raw_marker=raw_marker, duplicate_raw=duplicate_raw)
    data.extend(bytes(max(0, 512 - len(data))))
    if boundary and mode == "single":
        map_offset = 32 + second_offset + len(second)
        data.extend(bytes(map_offset + 1 + 1024 * 1024 - len(data)))
    return bytes(data), bytes(boot)


@pytest.mark.parametrize("mode", ["fragments", "single", "overlap", "large_stream"])
def test_sector_map_reads_both_blocks(peare_lib, tmp_path, mode):
    data, boot = _archive(mode)
    path = tmp_path / "fragments.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, 512) == boot
            assert peare_lib.read_resource(resource, 504, 16) == boot[-8:] + b"B" * 8
            assert peare_lib.read_resource(resource, 512, 512) == b"B" * 512
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("mode", ["single", "fragments"])
@pytest.mark.parametrize("order", [(2, 3, 4, 6), (6, 4, 3, 2), (3, 2, 6, 4)])
@pytest.mark.parametrize("sector_width", [2, 3])
@pytest.mark.parametrize("boundary", [False, True])
def test_sector_map_tlv_discovery(peare_lib, tmp_path, mode, order, sector_width, boundary):
    data, boot = _archive(mode, order, sector_width, boundary)
    path = tmp_path / "tlv-order.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, 512) == boot
            assert peare_lib.read_resource(resource, 512, 512) == b"B" * 512
        finally:
            opener._L.peare_resource_destroy(resource)


def test_sector_map_discovery_skips_invalid_candidate(peare_lib, tmp_path):
    data, boot = _archive("single", order=(6, 3, 2, 4), sector_width=3)
    decoy = _map([(0, 1)], order=(3, 6, 2, 4), sector_width=3)
    path = tmp_path / "decoy.tib"
    path.write_bytes(data + decoy[:-1] + bytes([decoy[-1] ^ 1]))
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, 512) == boot
            assert peare_lib.read_resource(resource, 512, 512) == b"B" * 512
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("mode", ["single", "fragments"])
def test_sector_map_skips_extended_unknown_field(peare_lib, tmp_path, mode):
    data, boot = _archive(mode, extra_field=bytes(range(64)))
    path = tmp_path / "unknown-field.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, 512) == boot
            assert peare_lib.read_resource(resource, 512, 512) == b"B" * 512
        finally:
            opener._L.peare_resource_destroy(resource)


def test_raw_codec_marker_rejects_payload(peare_lib, tmp_path):
    data, _ = _archive("raw", raw_marker=b"\x01")
    path = tmp_path / "raw-marker.tib"
    path.write_bytes(data)
    assert peare_lib.open_file(str(path)) is None


def test_raw_codec_marker_rejects_duplicate(peare_lib, tmp_path):
    data, _ = _archive("raw", duplicate_raw=True)
    path = tmp_path / "raw-marker-duplicate.tib"
    path.write_bytes(data)
    assert peare_lib.open_file(str(path)) is None


@pytest.mark.parametrize("mode", ["geometry", "duplicate"])
def test_sector_fragmented_map_rejects_ambiguity(peare_lib, tmp_path, mode):
    data, _ = _archive(mode)
    path = tmp_path / "overlap.tib"
    path.write_bytes(data)
    assert peare_lib.open_file(str(path)) is None


def test_sector_read_failure_preserves_output(peare_lib, tmp_path):
    data, _ = _archive("corrupt_second")
    path = tmp_path / "corrupt-second.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, 504, output, 16, C.byref(count))
            assert status != 0 and count.value == 0
            assert bytes(output) == bytes([0x5a] * 16)
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("mode", ["dedup_inline", "dedup_reference"])
def test_sector_dedup_block(peare_lib, tmp_path, mode):
    data, boot = _archive(mode)
    path = tmp_path / "dedup-sector.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, len(boot)) == boot
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, len(boot) - 8, output, 16, C.byref(count))
            if mode == "dedup_inline":
                assert status == 0 and count.value == 16
                assert bytes(output) == boot[-8:] + b"B" * 8
            else:
                assert status != 0 and count.value == 0
                assert bytes(output) == bytes([0x5a] * 16)
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("corrupt", [False, True])
@pytest.mark.parametrize("store_name", ["unified_data.ds.0", "disk_data.ds.0", "disk_data.ds.1"])
@pytest.mark.parametrize("size_mode", ["exact", "extra", "oversized", "max", "truncated", "missing_szh"])
def test_sector_dedup_reference_provider(peare_lib, tmp_path, corrupt, store_name, size_mode):
    import hashlib

    payload = b"C" * 4096
    digest = hashlib.md5(payload).digest()
    archive, boot = _archive("dedup_reference", reference_hash=digest)
    path = tmp_path / "dedup-sector.tib"
    path.write_bytes(archive)
    stored_payload = (b"X" + payload[1:]) if corrupt else payload
    store = bytes(4) + stored_payload + digest + b"\x00" + struct.pack("<QQ", len(payload), len(payload))
    (tmp_path / store_name).write_bytes(store[:-1] if size_mode == "truncated" else store)
    declared = (2**64 - 1 if size_mode == "max" else
                len(store) + (8 if size_mode == "oversized" else 0))
    if size_mode != "missing_szh":
        (tmp_path / (store_name + ".szh")).write_bytes(
            struct.pack("<Q", declared) +
            (b"extra" if size_mode == "extra" else b""))
    if store_name.startswith("disk_"):
        other = b"unrelated"
        other_store = bytes(4) + other + hashlib.md5(other).digest() + b"\x00" + struct.pack("<QQ", len(other), len(other))
        (tmp_path / "unified_data.ds.0").write_bytes(other_store)
        (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(other_store)))
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            if corrupt or size_mode == "truncated":
                output = (C.c_uint8 * 16)(*([0x5a] * 16))
                count = C.c_size_t(99)
                status = opener._L.peare_resource_read(resource, len(boot) - 8, output, 16, C.byref(count))
                assert status != 0 and count.value == 0
                assert bytes(output) == bytes([0x5a] * 16)
            else:
                assert peare_lib.read_resource(resource, len(boot) - 8, 16) == boot[-8:] + b"C" * 8
                assert peare_lib.read_resource(resource, len(boot), len(payload)) == payload
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("other_kind", ["invalid_size", "invalid_record", "corrupt_duplicate", "valid_duplicate", "extended_size"])
def test_sector_dedup_multiple_store_integrity(peare_lib, tmp_path, other_kind):
    import hashlib

    payload = b"C" * 4096
    digest = hashlib.md5(payload).digest()
    archive, boot = _archive("dedup_reference", reference_hash=digest)
    path = tmp_path / "multiple-dedup.tib"
    path.write_bytes(archive)
    good = bytes(4) + payload + digest + b"\0" + struct.pack("<QQ", len(payload), len(payload))
    (tmp_path / "unified_data.ds.0").write_bytes(good)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(good)))
    if other_kind == "invalid_record":
        other = bytes(5)
    elif other_kind == "corrupt_duplicate":
        other = bytes(4) + b"X" + payload[1:] + digest + b"\0" + struct.pack("<QQ", len(payload), len(payload))
    else:
        other = good
    (tmp_path / "disk_data.ds.0").write_bytes(other)
    size_data = b"bad" if other_kind == "invalid_size" else struct.pack("<Q", len(other))
    if other_kind == "extended_size":
        size_data += b"extra"
    (tmp_path / "disk_data.ds.0.szh").write_bytes(size_data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            if other_kind == "corrupt_duplicate":
                output = (C.c_uint8 * 16)(*([0x5a] * 16))
                count = C.c_size_t(99)
                status = opener._L.peare_resource_read(resource, len(boot), output, 16, C.byref(count))
                assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
            else:
                assert peare_lib.read_resource(resource, len(boot), len(payload)) == payload
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("damage", ["missing", "corrupt"])
def test_sector_dedup_missing_reference_preserves_inline_reads(peare_lib, tmp_path, damage):
    import hashlib

    archive, boot = _archive("dedup_mixed")
    path = tmp_path / "dedup-sector.tib"
    path.write_bytes(archive)
    payload = b"unrelated" if damage == "missing" else b"X" * 4096
    digest = hashlib.md5(payload).digest() if damage == "missing" else bytes(16)
    store = bytes(4) + payload + digest + b"\x00" + struct.pack("<QQ", len(payload), len(payload))
    (tmp_path / "unified_data.ds.0").write_bytes(store)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, len(boot), 16) == b"B" * 16
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, len(boot) + 4096 - 8,
                                                   output, 16, C.byref(count))
            assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
            assert peare_lib.read_resource(resource, len(boot), 16) == b"B" * 16
        finally:
            opener._L.peare_resource_destroy(resource)


def test_sector_dedup_mixed_reference_provider(peare_lib, tmp_path):
    import hashlib

    payload = b"C" * 4096
    digest = hashlib.md5(payload).digest()
    archive, boot = _archive("dedup_mixed", reference_hash=digest)
    path = tmp_path / "dedup-sector.tib"
    path.write_bytes(archive)
    store = bytes(4) + payload + digest + b"\x00" + struct.pack("<QQ", len(payload), len(payload))
    (tmp_path / "unified_data.ds.0").write_bytes(store)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, len(boot) + 4096 - 8, 16) == b"B" * 8 + b"C" * 8
            assert peare_lib.read_resource(resource, len(boot) + 4096, 4096) == payload
            assert peare_lib.read_resource(resource, len(boot), 4096) == b"B" * 4096
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("damage", ["missing", "corrupt"])
def test_sector_dedup_multiple_references_do_not_reuse_data(peare_lib, tmp_path, damage):
    import hashlib

    payloads = {i: bytes([i]) * 4096 for i in (0, 8, 15)}
    hashes = {i: hashlib.md5(data).digest() for i, data in payloads.items()}
    archive, boot = _archive("dedup_many_refs", reference_hashes=hashes)
    path = tmp_path / "dedup-sector.tib"
    path.write_bytes(archive)
    store = bytearray(4)
    for index in (0, 8, 15):
        if index == 15 and damage == "missing":
            continue
        data = payloads[index]
        if index == 15 and damage == "corrupt":
            data = b"X" + data[1:]
        store.extend(data + hashes[index] + b"\x00" + struct.pack("<QQ", len(data), len(data)))
    (tmp_path / "unified_data.ds.0").write_bytes(store)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, len(boot), 16) == bytes(16)
            assert peare_lib.read_resource(resource, len(boot) + 8 * 4096, 16) == bytes([8]) * 16
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, len(boot) + 15 * 4096,
                                                   output, 16, C.byref(count))
            assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
            assert peare_lib.read_resource(resource, len(boot) + 8 * 4096, 16) == bytes([8]) * 16
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("damage", ["none", "missing", "corrupt"])
def test_sector_dedup_external_boot_provider(peare_lib, tmp_path, damage):
    import hashlib

    _, boot = _archive("dedup_inline")
    digest = hashlib.md5(boot).digest()
    archive, _ = _archive("dedup_inline", boot_reference=digest)
    path = tmp_path / "dedup-sector.tib"
    path.write_bytes(archive)
    stored_boot = boot if damage != "corrupt" else b"X" + boot[1:]
    store_hash = digest if damage != "missing" else bytes(16)
    store = bytes(4) + stored_boot + store_hash + b"\x00" + struct.pack("<QQ", len(boot), len(boot))
    (tmp_path / "unified_data.ds.0").write_bytes(store)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))
    opener = peare_lib.open_file(str(path))
    if damage != "none":
        assert opener is None
        return
    assert opener is not None
    with opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, len(boot)) == boot
            assert peare_lib.read_resource(resource, len(boot), 16) == b"B" * 16
        finally:
            opener._L.peare_resource_destroy(resource)


def test_sector_dedup_fat_gui(peare_gui_path, tmp_path):
    import hashlib
    import json
    import os
    import subprocess
    import sys
    from pathlib import Path

    sample_root = os.environ.get("PEARE_ACRONIS_SAMPLES")
    if not sample_root:
        pytest.skip("Set PEARE_ACRONIS_SAMPLES to the independent sample directory")
    sample = Path(sample_root) / "unicode-fat-pyfatfs.img"
    if not sample.is_file():
        pytest.skip("Independent FAT sample not available")
    image = sample.read_bytes()
    assert len(image) % 4096 == 0
    base, _ = _archive("dedup_inline")
    archive = bytearray(base[:32])
    store = bytearray(4)
    hashes = set()
    records = []
    for offset in range(0, len(image), 4096):
        data = image[offset:offset + 4096]
        digest = hashlib.md5(data).digest()
        chunk = b"\x01" + zlib.compress(b"\x01" + digest)
        records.append((len(archive) - 32, len(chunk)))
        archive.extend(chunk)
        if digest not in hashes:
            store.extend(data + digest + b"\x00" + struct.pack("<QQ", len(data), len(data)))
            hashes.add(digest)
    map_offset = len(archive) - 32
    archive.extend(_map(records, cluster_sectors=8, dedup=True))
    metadata_offset = len(archive) - 32
    fields = ((0, map_offset), (2, 512), (3, 8), (0x11, 0), (0x12, len(image) // 512), (0x45, 1))
    body = b"".join(_tlv(tag, value) for tag, value in fields)
    archive.extend(struct.pack("<H", len(body) + 2) + body)
    locator = _tlv(0, metadata_offset)
    archive.extend(locator + struct.pack("<II", len(locator), 0x94e18a2b))
    footer = bytearray(48)
    struct.pack_into("<Q", footer, 8, len(archive) - 32)
    struct.pack_into("<I", footer, 44, 0xce24b9a2)
    archive.extend(footer)
    archive_path = tmp_path / "d4-fat.tib"
    archive_path.write_bytes(archive)
    (tmp_path / "unified_data.ds.0").write_bytes(store)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))

    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    plugin = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    configured = environment.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    project = Path(__file__).resolve().parents[1]
    candidates = ([Path(configured)] if configured else []) + [
        project / "build-windows" / "test-plugins" / "platforms",
        peare_gui_path.parent / "platforms",
    ]
    plugin_dir = next((p for p in candidates if (p / plugin).is_file()), None)
    assert plugin_dir is not None, "Offscreen Qt plugin is missing"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    screenshot = tmp_path / "preview.png"
    result = subprocess.run(
        [str(peare_gui_path), "--preview-resource", str(archive_path),
         "Partition 1/name_\U0001f680.txt", str(screenshot)],
        capture_output=True, text=True, encoding="utf-8", errors="replace", env=environment,
        timeout=90, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    line = next(line for line in result.stdout.splitlines() if line.startswith("GUI_INSPECTION "))
    report = json.loads(line[len("GUI_INSPECTION "):])
    assert report["leaf"] and not report["error"] and not report["duplicate_paths"]
    assert report["sha256"] == hashlib.sha256(b"independent FAT sample\n").hexdigest()
    assert screenshot.is_file()


@pytest.mark.parametrize("mode", ["dedup_mixed", "dedup_many", "dedup_sparse", "raw_dedup_mixed"])
def test_sector_dedup_inline_ranges(peare_lib, tmp_path, mode):
    data, boot = _archive(mode)
    path = tmp_path / "dedup-ranges.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, len(boot)) == boot
            expected = bytes(4096) if mode == "dedup_sparse" else b"B" * 4096
            assert peare_lib.read_resource(resource, len(boot), 4096) == expected
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, len(boot) + 4096 - 8, output, 16, C.byref(count))
            if mode in ("dedup_mixed", "raw_dedup_mixed"):
                assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
                assert peare_lib.read_resource(resource, len(boot), 16) == b"B" * 16
            elif mode == "dedup_sparse":
                assert status == 0 and count.value == 16 and bytes(output) == bytes(8) + b"B" * 8
                assert peare_lib.read_resource(resource, len(boot) + 4096, 4096) == b"B" * 4096
            else:
                assert status == 0 and count.value == 16 and bytes(output) == b"B" * 16
                assert peare_lib.read_resource(resource, len(boot) * 2 - 16, 16) == b"B" * 16
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("mode", ["raw", "raw_dedup", "raw_bad_checksum"])
def test_sector_raw_codec(peare_lib, tmp_path, mode):
    data, boot = _archive(mode)
    path = tmp_path / "raw-sector.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, len(boot) - 8, output, 16, C.byref(count))
            if mode == "raw_bad_checksum":
                assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
            else:
                assert status == 0 and count.value == 16
                assert bytes(output) == boot[-8:] + b"B" * 8
        finally:
            opener._L.peare_resource_destroy(resource)


def test_sector_dedup_multibyte_mixed_bitmap(peare_lib, tmp_path):
    data, boot = _archive("dedup_many_refs")
    path = tmp_path / "dedup-multibyte.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            for index in (0, 8, 15):
                output = (C.c_uint8 * 16)(*([0x5a] * 16))
                count = C.c_size_t(99)
                status = opener._L.peare_resource_read(resource, len(boot) + index * 4096, output, 16, C.byref(count))
                assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
            for index in (1, 7, 9, 14):
                assert peare_lib.read_resource(resource, len(boot) + index * 4096, 32) == bytes([index]) * 32
        finally:
            opener._L.peare_resource_destroy(resource)


def test_sector_dedup_sparse_small_clusters(peare_lib, tmp_path):
    data, boot = _archive("dedup_small_sparse")
    path = tmp_path / "dedup-small-sparse.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, 512) == boot[:512]
            assert peare_lib.read_resource(resource, len(boot), 512) == b"B" * 512
            assert peare_lib.read_resource(resource, len(boot) + 512, 512) == bytes(512)
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, len(boot) + 16 * 512, output, 16, C.byref(count))
            assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
            assert peare_lib.read_resource(resource, len(boot) + 17 * 512, 512) == bytes(512)
            assert peare_lib.read_resource(resource, len(boot) + 14 * 512, 512) == b"B" * 512
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("mode", ["dedup_bad_ref", "dedup_bad_adler", "dedup_trailing", "raw_dedup_bad_checksum"])
def test_sector_dedup_corrupt_data_preserves_output(peare_lib, tmp_path, mode):
    data, boot = _archive(mode)
    path = tmp_path / "dedup-corrupt.tib"
    path.write_bytes(data)
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, len(boot) - 8, output, 16, C.byref(count))
            assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
        finally:
            opener._L.peare_resource_destroy(resource)
