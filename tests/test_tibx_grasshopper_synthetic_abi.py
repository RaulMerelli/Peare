"""Synthetic TIBX Grasshopper-CBC vectors, not authentic archives."""

import ctypes as C
import hashlib
import struct
import time
import zlib

import pytest

from peare_opener import _HANDLE
from test_tibx_abi import _directory_source, _open_source_with_password, _resource_hash
from test_tibx_synthetic_abi import _commit_crc, _crc32c, _page, _section, _tree


# RFC 7801 sections 5.4-5.6, block encryption and decryption example.
RFC_KEY = bytes.fromhex("8899aabbccddeeff0011223344556677fedcba98765432100123456789abcdef")
RFC_PLAINTEXT = bytes.fromhex("1122334455667700ffeeddccbbaa9988")
RFC_CIPHERTEXT = bytes.fromhex("7f679d90bebc24305a468d42b9d4edcd")
PASSWORD = "peare-test"
SALT = bytes(range(16))
# AES-256-CBC/zero-IV wrapping of RFC_KEY + full PKCS#7 block, independently generated.
WRAPPED = bytes.fromhex(
    "d8a4d0945c1c4e4a48e3ac45e0075e1a0a3200b3f8c75629fee598b120f4dcfa"
    "65099bb327315341ec27b7cbd34c7036"
)
BENCH_BLOCKS = 16384


def _rfc_cbc(plaintext):
    assert len(plaintext) < 16
    padding = 16 - len(plaintext)
    padded = plaintext + bytes([padding]) * padding
    iv = bytes(a ^ b for a, b in zip(RFC_PLAINTEXT, padded))
    return iv + RFC_CIPHERTEXT


def _archive(case, wrapped=WRAPPED):
    plain = b"GOST"
    codec = 0
    if case in ("zlib", "bad_adler"):
        plain = b"x"
        packed = zlib.compress(plain)
        assert len(packed) < 16
        if case == "bad_adler":
            packed = packed[:-1] + bytes([packed[-1] ^ 1])
        codec = 2
    else:
        packed = plain
    if case == "benchmark":
        # The last block decrypts to a complete padding block. Only the preceding
        # RFC-derived prefix is used as an expected value in the benchmark.
        previous = bytes(byte ^ 16 for byte in RFC_PLAINTEXT)
        encrypted = bytearray(RFC_PLAINTEXT + RFC_CIPHERTEXT * BENCH_BLOCKS + previous + RFC_CIPHERTEXT)
        logical_size = (BENCH_BLOCKS + 1) * 16
    else:
        encrypted = bytearray(_rfc_cbc(packed))
        logical_size = len(plain)
    if case == "bad_padding":
        encrypted[15] ^= 16 - len(packed)
    elif case == "truncated":
        encrypted.pop()
    elif case == "cbc_malleability":
        # The enclosing synthetic page is checksummed after this IV change.
        encrypted[0] ^= 1

    item = (struct.pack(">I", 3) + b"\0\3\0disk1/part1",
            b"\0\0\1" + struct.pack(">QQ", 41, logical_size))
    imap = _tree([item], 0, 0)
    extent = (struct.pack(">QQ", 41, 0) + logical_size.to_bytes(3, "big") + struct.pack(">IQ", 3, 0),
              struct.pack(">QH", 52, 0xffff))
    if case in ("empty_inline", "empty_inline_bad_padding"):
        dmap = bytearray(_tree([], 31, 10)[:376])
        dmap[344] = 0x80
        encrypted_inline = bytearray(_rfc_cbc(b""))
        if case == "empty_inline_bad_padding":
            encrypted_inline[15] ^= 16
        struct.pack_into(">I", dmap, 348, len(encrypted_inline))
        dmap.extend(encrypted_inline)
    else:
        dmap = _tree([extent], 31, 10)
    segment = bytearray(36)
    segment[:4] = b"SE\0\1"
    struct.pack_into(">III", segment, 4, logical_size, len(encrypted), 1)
    segment[16] = codec
    pages = [_page(255, segment + encrypted[:4052])]
    pages.extend(_page(255, encrypted[pos:pos + 4088]) for pos in range(4052, len(encrypted), 4088))
    assert len(pages) <= 255
    segments = _tree([(struct.pack(">Q", 52), struct.pack(">Q", (len(pages) << 56) | 4) + bytes(24))], 8, 32)
    point = bytearray(132)
    struct.pack_into(">I", point, 48, 2)
    slices = _tree([(struct.pack(">I", 3), point)], 4, 132)
    keymap = _tree([(b"\1", bytes((1, 4, 10, 0)) + SALT + wrapped)], 0, 0)

    header = bytearray(1024)
    header[:4] = b"ARCH"
    struct.pack_into(">H", header, 8, 8)
    header[32:48] = bytes(range(16))
    for section in (imap, dmap, segments, b"", b"", slices, b"", keymap):
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


@pytest.mark.parametrize("length,wrapped", [
    (64, "43d2d230209e20f7238855d07c414592d72b526a5bcd23486186b01c08f77857645ecff1d666b3749f54bd6d735b1633"),
    (65, "8c63adb53d7914934b18b14a4d600e4dd70478fa203ad043f983e9a7fe823a712b69e2402bdcdada6569b772b00d32fe"),
    (80, "b6847171bf1aab7e94cccd52d9cb4516ab40633524d3208e375f94c86c7c627000d39c93ec36e68fa5bdad645e80ed8e"),
])
def test_synthetic_tibx_long_password_key_unwrap(peare_lib, tmp_path, length, wrapped):
    password = "L" * length
    path = tmp_path / "long-password.tibx"
    path.write_bytes(_archive("stored", bytes.fromhex(wrapped)))
    with peare_lib.open_file(str(path), password) as opener:
        assert opener is not None
        assert _resource_hash(opener, "disk1/part1", password) == hashlib.sha256(b"GOST").hexdigest()


@pytest.mark.parametrize("case,expected", [
    ("stored", b"GOST"), ("zlib", b"x"), ("empty_inline", bytes(4)),
    ("cbc_malleability", b"FOST"),
    ("bad_padding", None), ("truncated", None), ("bad_adler", None), ("empty_inline_bad_padding", None),
])
def test_synthetic_tibx_grasshopper_rfc_block_abi(peare_lib, tmp_path, case, expected):
    path = tmp_path / "synthetic-grasshopper.tibx"
    path.write_bytes(_archive(case))
    opener = peare_lib.open_file(str(path), PASSWORD)
    assert opener is not None, peare_lib.status_message(peare_lib.last_open_status)
    directory = None
    resource = _HANDLE()
    try:
        source = _directory_source(opener, "disk1")
        try:
            status, directory = _open_source_with_password(peare_lib, source, PASSWORD)
        finally:
            peare_lib._lib.peare_source_destroy(source)
        assert status == 0 and directory is not None
        find = directory._L.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        status = find(directory._h, b"TIBX_VOLUME", b"part1", None, C.byref(resource))
        if status != 0:
            assert expected is None and case == "empty_inline_bad_padding"
            return
        output = (C.c_uint8 * 4)()
        count = C.c_size_t()
        status = directory._L.peare_resource_read(resource, 0, output, 4, C.byref(count))
        if expected is None:
            assert status != 0 and count.value == 0
        else:
            assert status == 0 and bytes(output[:count.value]) == expected
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if directory is not None:
            directory.close()
        opener.close()


def test_synthetic_tibx_grasshopper_wrong_password(peare_lib, tmp_path):
    path = tmp_path / "synthetic-grasshopper.tibx"
    path.write_bytes(_archive("stored"))
    assert hashlib.pbkdf2_hmac("sha256", PASSWORD.encode(), SALT, 1 << 10).hex() == (
        "735b5e66db72c0d0ba879aed629bad565091e6c0cd509e04eafee8ed0d960eb4")
    assert peare_lib.open_file(str(path)) is None
    assert peare_lib.last_open_status == 14
    assert peare_lib.open_file(str(path), "wrong") is None
    assert peare_lib.last_open_status == 15


def test_synthetic_tibx_grasshopper_multiblock(peare_lib, tmp_path):
    path = tmp_path / "synthetic-grasshopper-benchmark.tibx"
    path.write_bytes(_archive("benchmark"))
    opened_at = time.perf_counter()
    root = peare_lib.open_file(str(path), PASSWORD)
    assert root is not None
    child, resource = None, _HANDLE()
    try:
        source = _directory_source(root, "disk1")
        try:
            status, child = _open_source_with_password(peare_lib, source, PASSWORD)
        finally:
            peare_lib._lib.peare_source_destroy(source)
        assert status == 0 and child is not None
        find = child._L.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        started = time.perf_counter()
        assert find(child._h, b"TIBX_VOLUME", b"part1", None, C.byref(resource)) == 0
        first = peare_lib.read_resource(resource, 0, 4096)
        cold = time.perf_counter() - started
        started = time.perf_counter()
        again = peare_lib.read_resource(resource, 0, 4096)
        warm = time.perf_counter() - started
        total = time.perf_counter() - opened_at
        recurring = bytes(a ^ b for a, b in zip(RFC_PLAINTEXT, RFC_CIPHERTEXT))
        expected = bytes(16) + recurring * 255
        assert first == expected and again == expected
        print(f"Grasshopper CBC synthetic segment: {(BENCH_BLOCKS + 1) * 16} bytes; "
              f"total {total:.6f}s; resource open and first read {cold:.6f}s; cached read {warm:.6f}s")
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if child is not None:
            child.close()
        root.close()
