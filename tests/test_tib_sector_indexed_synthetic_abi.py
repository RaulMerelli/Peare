"""Parser-derived synthetic vectors, not evidence of an actual TIB layout.

In particular, indexing the last of several map fragments is hypothetical.
The parser's separate non-indexed fragment scanner does not establish that
real indexed archives use this arrangement.
"""

import ctypes as C
import struct
import zlib

import pytest

from peare_opener import _Context, _HANDLE
from test_tib_sector_fragments_synthetic_abi import _map, _tlv


def _archive(fragmented=False, duplicate_index=False, single_volume=False, map_size_adjust=None,
             extra_frame=False, duplicate_map_size=False, external_chunks=False, dedup_size_adjust=0,
             external_indices=(1,), readable_sectors=None, declared_sectors=None, external_hashes=None):
    header = bytearray(32)
    struct.pack_into("<IHHQIII", header, 0, 0xa2b924ce, 32, 0, 1, 1, 1, 0)
    struct.pack_into("<I", header, 28, 32)
    struct.pack_into("<I", header, 24, zlib.adler32(header))
    image = bytearray(header)
    expected = {}
    frames = []
    # Deliberately out of index order: the ABI should expose sorted identifiers.
    entries = ((7, b"ABCDE" if external_chunks else b"AB"),) if single_volume else ((7, b"AB"), (2, b"XY"))
    for index, letters in entries:
        blocks = [bytes([letter]) * 512 for letter in letters]
        records = []
        for number, block in enumerate(blocks):
            if external_chunks and number in external_indices:
                records.append((None, 17))
                continue
            stored = b"\x01" + zlib.compress(block)
            records.append((len(image) - 32, len(stored)))
            image.extend(stored)
            if fragmented and index == 7 and number == 0:
                image.extend(_map(records))
        map_offset = len(image) - 32
        map_bytes = _map(records[-1:] if fragmented and index == 7 else records,
                         external_chunks=external_chunks)
        image.extend(map_bytes)
        frame_index = 7 if duplicate_index else index
        fields = [(0, map_offset), (2, 512), (3, 1), (0x11, 0),
                  (0x12, declared_sectors if declared_sectors is not None else len(blocks)), (0x45, frame_index)]
        if readable_sectors is not None:
            fields.append((0x13, readable_sectors))
        if external_chunks:
            fields += [(0x5d, len(image) - 32), (0x5e, 16 * len(records) + dedup_size_adjust)]
            image.extend(b"".join((external_hashes or {}).get(i, bytes(16)) for i in range(len(records))))
        if map_size_adjust is not None:
            fields.insert(1, (1, len(map_bytes) + map_size_adjust))
            if duplicate_map_size:
                fields.insert(2, (1, len(map_bytes) + map_size_adjust))
        body = b"".join(_tlv(tag, value) for tag, value in fields)
        frames.append(struct.pack("<H", len(body) + 2) + body)
        expected[f"Partition {index}"] = blocks
    image.extend(bytes(max(0, 512 - len(image))))
    metadata_offset = len(image) - 32
    if extra_frame:
        body = struct.pack("<HB", 1, 16) + bytes(range(16))
        image.extend(struct.pack("<H", len(body) + 2) + body)
    image.extend(b"".join(frames))
    locator = _tlv(0, metadata_offset)
    image.extend(locator)
    image.extend(struct.pack("<II", len(locator), 0x94e18a2b))
    slice_size = len(image) - 32
    footer = bytearray(48)
    struct.pack_into("<Q", footer, 8, slice_size)
    struct.pack_into("<I", footer, 44, 0xce24b9a2)
    image.extend(footer)
    return bytes(image), expected


def _read_volumes(peare_lib, path):
    result = {}
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = _HANDLE()
                assert opener._L.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)) == 0
                try:
                    context = _Context()
                    assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
                    try:
                        name = context.identifier_utf8.to_bytes().decode("utf-8")
                        size = int(context.data_size)
                    finally:
                        opener._L.peare_resource_context_free(C.byref(context))
                    assert name not in result
                    first = peare_lib.read_resource(resource, 0, 512)
                    try:
                        last = peare_lib.read_resource(resource, size - 512, 512)
                    except RuntimeError as error:
                        last = str(error)
                    result[name] = (size, first, last)
                finally:
                    opener._L.peare_resource_destroy(resource)
    return result


def test_indexed_complete_maps_and_identifiers(peare_lib, tmp_path):
    archive, expected = _archive()
    path = tmp_path / "indexed.tib"
    path.write_bytes(archive)
    volumes = _read_volumes(peare_lib, path)
    assert list(volumes) == ["Partition 2", "Partition 7"]
    for name, blocks in expected.items():
        assert volumes[name] == (1024, blocks[0], blocks[-1])


@pytest.mark.parametrize("limit", [0, 1, 2, 3])
def test_indexed_readable_sector_limit(peare_lib, tmp_path, limit):
    archive, _ = _archive(single_volume=True, readable_sectors=limit)
    path = tmp_path / "indexed-readable-limit.tib"
    path.write_bytes(archive)
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            for position, expected in [(0, b"A" * 16), (512, b"B" * 16)]:
                output = (C.c_uint8 * 16)(*([0x5a] * 16))
                count = C.c_size_t(99)
                status = opener._L.peare_resource_read(resource, position, output, 16, C.byref(count))
                if position // 512 < limit:
                    assert status == 0 and count.value == 16 and bytes(output) == expected
                else:
                    assert status != 0 and count.value == 0 and bytes(output) == b"\x5a" * 16
            if limit == 1:
                output = (C.c_uint8 * 16)(*([0x5a] * 16))
                count = C.c_size_t(99)
                status = opener._L.peare_resource_read(resource, 504, output, 16, C.byref(count))
                assert status != 0 and count.value == 0 and bytes(output) == b"\x5a" * 16
        finally:
            opener._L.peare_resource_destroy(resource)


def test_indexed_out_of_range_block_does_not_wrap(peare_lib, tmp_path):
    archive, _ = _archive(single_volume=True, declared_sectors=(1 << 32) + 1)
    path = tmp_path / "indexed-large-offset.tib"
    path.write_bytes(archive)
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            opener._L.peare_resource_read(resource, (1 << 32) * 512, output, 16, C.byref(count))
            assert count.value == 0 and bytes(output) == b"\x5a" * 16
            assert peare_lib.read_resource(resource, 0, 16) == b"A" * 16
        finally:
            opener._L.peare_resource_destroy(resource)


def test_indexed_external_chunk_preserves_later_offsets(peare_lib, tmp_path):
    archive, _ = _archive(single_volume=True, external_chunks=True)
    path = tmp_path / "indexed-external.tib"
    path.write_bytes(archive)
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, 512) == b"A" * 512
            assert peare_lib.read_resource(resource, 1024, 512) == b"C" * 512
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, 504, output, 16, C.byref(count))
            assert status != 0 and count.value == 0
            assert bytes(output) == bytes([0x5a] * 16)
            assert peare_lib.read_resource(resource, 1024, 512) == b"C" * 512
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("damage", ["missing", "corrupt"])
def test_indexed_external_provider_failure_preserves_other_chunks(peare_lib, tmp_path, damage):
    import hashlib

    archive, _ = _archive(single_volume=True, external_chunks=True)
    path = tmp_path / "indexed-external.tib"
    path.write_bytes(archive)
    payload = b"unrelated" if damage == "missing" else b"X" * 513
    digest = hashlib.md5(payload).digest() if damage == "missing" else bytes(16)
    store = bytes(4) + payload + digest + b"\x00" + struct.pack("<QQ", len(payload), len(payload))
    (tmp_path / "unified_data.ds.0").write_bytes(store)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 0, 512) == b"A" * 512
            output = (C.c_uint8 * 16)(*([0x5a] * 16))
            count = C.c_size_t(99)
            status = opener._L.peare_resource_read(resource, 504, output, 16, C.byref(count))
            assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
            assert peare_lib.read_resource(resource, 1024, 512) == b"C" * 512
            assert peare_lib.read_resource(resource, 0, 512) == b"A" * 512
        finally:
            opener._L.peare_resource_destroy(resource)


def test_indexed_external_provider_reads_mixed_groups(peare_lib, tmp_path):
    import hashlib

    payloads = {1: b"\x01" + b"B" * 512, 2: b"\x01" + b"C" * 512}
    hashes = {index: hashlib.md5(data).digest() for index, data in payloads.items()}
    archive, _ = _archive(single_volume=True, external_chunks=True, external_indices=(1, 2),
                          external_hashes=hashes)
    path = tmp_path / "indexed-external-valid.tib"
    path.write_bytes(archive)
    store = bytearray(4)
    for index in (1, 2):
        payload = payloads[index]
        store.extend(payload + hashes[index] + b"\x00" + struct.pack("<QQ", len(payload), len(payload)))
    (tmp_path / "unified_data.ds.0").write_bytes(store)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            assert peare_lib.read_resource(resource, 504, 16) == b"A" * 8 + b"B" * 8
            assert peare_lib.read_resource(resource, 1016, 16) == b"B" * 8 + b"C" * 8
            assert peare_lib.read_resource(resource, 1528, 16) == b"C" * 8 + b"D" * 8
            assert peare_lib.read_resource(resource, 0, 5 * 512) == b"".join(bytes([c]) * 512 for c in b"ABCDE")
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("payload,valid", [
    (b"\x00", True), (b"\x80", False),
    (b"\x01" + b"B" * 511, False), (b"\x00extra", False),
])
def test_indexed_external_provider_bitmap_validation(peare_lib, tmp_path, payload, valid):
    import hashlib

    digest = hashlib.md5(payload).digest()
    archive, _ = _archive(single_volume=True, external_chunks=True, external_hashes={1: digest})
    path = tmp_path / "indexed-bitmap.tib"
    path.write_bytes(archive)
    store = bytes(4) + payload + digest + b"\x00" + struct.pack("<QQ", len(payload), len(payload))
    (tmp_path / "unified_data.ds.0").write_bytes(store)
    (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            if valid:
                assert peare_lib.read_resource(resource, 512, 512) == bytes(512)
            else:
                output = (C.c_uint8 * 16)(*([0x5a] * 16))
                count = C.c_size_t(99)
                status = opener._L.peare_resource_read(resource, 512, output, 16, C.byref(count))
                assert status != 0 and count.value == 0 and bytes(output) == bytes([0x5a] * 16)
            assert peare_lib.read_resource(resource, 1024, 512) == b"C" * 512
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("external_indices", [(0, 1), (1, 2), (3, 4)])
def test_indexed_external_chunks_do_not_shift_other_chunks(peare_lib, tmp_path, external_indices):
    archive, _ = _archive(single_volume=True, external_chunks=True, external_indices=external_indices)
    path = tmp_path / "indexed-external-sequence.tib"
    path.write_bytes(archive)
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            for index, letter in enumerate(b"ABCDE"):
                if index in external_indices:
                    output = (C.c_uint8 * 1)(0x5a)
                    count = C.c_size_t(99)
                    status = opener._L.peare_resource_read(resource, index * 512, output, 1, C.byref(count))
                    assert status != 0 and count.value == 0 and bytes(output) == b"\x5a"
                else:
                    assert peare_lib.read_resource(resource, index * 512, 512) == bytes([letter]) * 512
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("provider_mode", ["absent", "missing", "bad_bitmap", "sparse", "inline_sparse"])
def test_large_external_chunk_with_sparse_or_missing_data(peare_lib, tmp_path, provider_mode):
    import hashlib
    import subprocess
    import sys
    from pathlib import Path

    path = tmp_path / "indexed-large-reference.tib"
    # Keep the actual input tiny; only the declared logical extent is large.
    image = bytearray()
    header = bytearray(32)
    struct.pack_into("<IHHQIII", header, 0, 0xa2b924ce, 32, 0, 1, 1, 1, 0)
    struct.pack_into("<I", header, 28, 32)
    struct.pack_into("<I", header, 24, zlib.adler32(header))
    image.extend(header)
    if provider_mode == "inline_sparse":
        bitmap = bytearray((16383 + 7) // 8)
        bitmap[0] = 1
        bitmap[16382 // 8] |= 1 << (16382 % 8)
        payload = bytes(8) + b"Q" * (256 * 512) + b"R" * (256 * 512)
        block = bytes(bitmap) + zlib.compress(payload)
        image.extend(block)
        map_position = len(image) - 32
        map_bytes = _map([(0, len(block))], cluster_sectors=256, clusters_per_block=16383, dedup=True)
    else:
        map_position = 0
        map_bytes = _map([(None, 17)], cluster_sectors=256, clusters_per_block=16383, external_chunks=True)
    image.extend(map_bytes)
    table_position = len(image) - 32
    if provider_mode in ("sparse", "bad_bitmap"):
        bitmap = bytearray((16383 + 7) // 8)
        if provider_mode == "sparse":
            bitmap[0] |= 1
            bitmap[16382 // 8] |= 1 << (16382 % 8)
            payload = bytes(bitmap) + b"Q" * (256 * 512) + b"R" * (256 * 512)
        else:
            bitmap[-1] = 128
            payload = bytes(bitmap)
        digest = hashlib.md5(payload).digest()
        image.extend(digest)
    elif provider_mode != "inline_sparse":
        image.extend(bytes(16))
    image.extend(bytes(max(0, 512 - len(image))))
    metadata_position = len(image) - 32
    fields = [(0, map_position), (1, len(map_bytes)), (2, 512), (3, 256), (0x11, 0),
              (0x12, 256 * 16383), (0x45, 1)]
    if provider_mode != "inline_sparse":
        fields += [(0x5d, table_position), (0x5e, 16)]
    body = b"".join(_tlv(tag, value) for tag, value in fields)
    image.extend(struct.pack("<H", len(body) + 2) + body)
    locator = _tlv(0, metadata_position)
    image.extend(locator)
    image.extend(struct.pack("<II", len(locator), 0x94e18a2b))
    slice_size = len(image) - 32
    footer = bytearray(48)
    struct.pack_into("<Q", footer, 8, slice_size)
    struct.pack_into("<I", footer, 44, 0xce24b9a2)
    image.extend(footer)
    path.write_bytes(image)
    if provider_mode not in ("absent", "inline_sparse"):
        if provider_mode == "missing":
            payload = b"unrelated"
            digest = hashlib.md5(payload).digest()
        store = (bytes(4) + payload + digest + b"\x00" +
                 struct.pack("<QQ", len(payload), len(payload)))
        (tmp_path / "unified_data.ds.0").write_bytes(store)
        (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", len(store)))
    helper = Path(__file__).with_name("tib_large_sparse_probe.py")
    result = subprocess.run(
        [sys.executable, str(helper), peare_lib._lib._name, str(path), provider_mode],
        capture_output=True, text=True, timeout=30, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("adjust", [-1, 1])
def test_indexed_external_chunk_rejects_invalid_table_size(peare_lib, tmp_path, adjust):
    archive, _ = _archive(single_volume=True, external_chunks=True, dedup_size_adjust=adjust)
    path = tmp_path / "indexed-external-table.tib"
    path.write_bytes(archive)
    assert peare_lib.open_file(str(path)) is None


@pytest.mark.xfail(strict=True, raises=AssertionError,
                   reason="Hypothetical indexed fragments are not assembled by openIndexedVolumes")
def test_hypothetical_indexed_fragmented_map_reads_both_blocks(peare_lib, tmp_path):
    archive, expected = _archive(fragmented=True)
    path = tmp_path / "indexed-fragmented.tib"
    path.write_bytes(archive)
    volumes = _read_volumes(peare_lib, path)
    assert list(volumes) == ["Partition 2", "Partition 7"]
    assert volumes["Partition 2"] == (1024, *expected["Partition 2"])
    assert volumes["Partition 7"] == (1024, *expected["Partition 7"])


@pytest.mark.xfail(strict=True, raises=AssertionError,
                   reason="Hypothetical indexed fragments are not assembled by openIndexedVolumes")
def test_hypothetical_single_indexed_fragmented_map(peare_lib, tmp_path):
    archive, expected = _archive(fragmented=True, single_volume=True)
    path = tmp_path / "indexed-single-fragmented.tib"
    path.write_bytes(archive)
    volumes = _read_volumes(peare_lib, path)
    assert volumes["Partition 7"] == (1024, *expected["Partition 7"])


def test_indexed_duplicate_identifiers_rejected(peare_lib, tmp_path):
    archive, _ = _archive(duplicate_index=True)
    path = tmp_path / "indexed-duplicate.tib"
    path.write_bytes(archive)
    opener = peare_lib.open_file(str(path))
    if opener is not None:
        opener.close()
    assert opener is None


@pytest.mark.parametrize("adjust", [-1, 0, 1])
def test_indexed_map_declared_size(peare_lib, tmp_path, adjust):
    archive, expected = _archive(map_size_adjust=adjust)
    path = tmp_path / "indexed-map-size.tib"
    path.write_bytes(archive)
    if adjust == 0:
        volumes = _read_volumes(peare_lib, path)
        for name, blocks in expected.items():
            assert volumes[name] == (1024, *blocks)
    else:
        opener = peare_lib.open_file(str(path))
        if opener is not None:
            opener.close()
        assert opener is None


def test_indexed_unrelated_metadata_field(peare_lib, tmp_path):
    archive, expected = _archive(map_size_adjust=0, extra_frame=True)
    path = tmp_path / "indexed-extra-metadata.tib"
    path.write_bytes(archive)
    volumes = _read_volumes(peare_lib, path)
    for name, blocks in expected.items():
        assert volumes[name] == (1024, *blocks)


def test_indexed_duplicate_map_size_rejected(peare_lib, tmp_path):
    archive, _ = _archive(map_size_adjust=0, duplicate_map_size=True)
    path = tmp_path / "indexed-duplicate-map-size.tib"
    path.write_bytes(archive)
    opener = peare_lib.open_file(str(path))
    if opener is not None:
        opener.close()
    assert opener is None
