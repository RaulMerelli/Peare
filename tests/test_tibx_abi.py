import base64
import ctypes as C
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from peare_opener import Opener, _Context, _HANDLE


SAMPLES = os.environ.get("PEARE_ACRONIS_SAMPLES")
pytestmark = pytest.mark.skipif(not SAMPLES, reason="Set PEARE_ACRONIS_SAMPLES to the independent sample directory")


def test_acronis15_incremental_consolidation(peare_lib):
    sample_dir = Path(SAMPLES) / "tibx-retention-ab15-20261122"
    cases = {
        "retention-chain.tibx": {
            "2/C:/PeareTibxRetentionSrc/payload.bin": (491520, "29a73f89c5cb1adeecd199dcc59b9f54ff7bee73b56245c90300d0e94bdad4b5"),
            "3/C:/PeareTibxRetentionSrc/payload.bin": (17635, "c1723f7f29b224c42f26452c3efa8f80f6ee8500ee78513e0c0732ba55399f7d"),
            "4/C:/PeareTibxRetentionSrc/payload.bin": (491520, "29a73f89c5cb1adeecd199dcc59b9f54ff7bee73b56245c90300d0e94bdad4b5"),
            "4/C:/PeareTibxRetentionSrc/protocol.txt": (1358, "52cf86496f3859d0f3e58776eccff1d6589792004d2291d3b0ce8d7635be7278"),
        },
        "retention-compacted.tibx": {
            "C:/PeareTibxRetentionSrc/payload.bin": (491520, "29a73f89c5cb1adeecd199dcc59b9f54ff7bee73b56245c90300d0e94bdad4b5"),
            "C:/PeareTibxRetentionSrc/protocol.txt": (1358, "52cf86496f3859d0f3e58776eccff1d6589792004d2291d3b0ce8d7635be7278"),
        },
    }
    archive_hashes = {
        "retention-chain.tibx": "100b3a1d06c8166fd5aea96c667ab8ef9f0d2f0aa50e3f6ff677b076a8e13cf3",
        "retention-compacted.tibx": "101931a9225b081c5707db560c21fe5082c1d9cb1caab37366b6b96a9dd97807",
    }
    for sample, expected in cases.items():
        archive = sample_dir / sample
        if not archive.is_file():
            pytest.skip("Acronis 15 incremental consolidation samples not available")
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == archive_hashes[sample]
        with peare_lib.open_file(str(archive)) as opener:
            assert opener is not None
            rows = opener.normalized_tree(max_depth=8)
            found = {row["path"]: row for row in rows if row["path"] in expected}
            assert set(found) == set(expected), rows
            for path, (size, digest) in expected.items():
                assert found[path]["size"] == size
                assert _resource_hash(opener, path) == digest


def test_acronis15_disk_backup(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "diskmodern.tibx"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "8a231ed9edb433742888d77baed447c536ad4d8815b65f2bcbdf0a3a0d534b11"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert _resource_hash(opener, "disk1/part1", ranges=[(2 * 512, 251 * 512),
                                                              (253 * 512, 251 * 512)]) == [
            "e4ff0da6f56eaa47decc7e8fb23fef725a0bef9ef1a93ffb1cbaffbe9163b23a",
            "e4ff0da6f56eaa47decc7e8fb23fef725a0bef9ef1a93ffb1cbaffbe9163b23a",
        ]
        assert _resource_hash(opener, "disk1/part1", ranges=[(253 * 512, 512), (253 * 512 - 15, 542)]) == [
            "b28139df2cb76769bfe70e73a24664437189d3244fbf0068a9d47ea6f9883688",
            "86099f4e9c3f9145826cc8b04f3caf8afaa70f04c310eb20224e96a15b21d733",
        ]
        assert _resource_hash(opener, "disk1/part1/DATA/CHANGE.TXT") == (
            "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765"
        )
        assert _resource_hash(opener, "disk1/part1/NOTEPAD.EXE") == (
            "c05fd6f341b2bcb5e482deff5baf53903910a42d55e4b2f23a7053cae5ddbdae"
        )


def test_acronis15_disk_backup_filtered_fat_directory(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "diskmodern.tibx"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "8a231ed9edb433742888d77baed447c536ad4d8815b65f2bcbdf0a3a0d534b11"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        rows = opener.normalized_tree(max_depth=4)
        names = {row["path"].rsplit("/", 1)[-1].upper() for row in rows
                 if row["path"].startswith("disk1/part1/") and row["path"].count("/") == 2}
        assert names == {"DATA", "NOTEPAD.EXE"}
        nested = {row["path"].rsplit("/", 1)[-1].upper() for row in rows
                  if row["path"].startswith("disk1/part1/DATA/")}
        assert nested == {"CHANGE.TXT"}


@pytest.mark.parametrize("part", ["part2", "part4"])
def test_acronis_gpt_fat_directories_match_listing(peare_lib, part):
    path = Path(SAMPLES) / "t1_disk_gpt.tibx"
    if not path.is_file():
        pytest.skip("Independent TIBX GPT backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "4023f69b9ec43a4cdc389b00b070a2da55334b36adc6c8dc27fc7d5b4ff549a5"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        prefix = "disk1/" + part + "/"
        rows = opener.normalized_tree(max_depth=4)
        names = {row["path"].rsplit("/", 1)[-1].upper() for row in rows
                 if row["path"].startswith(prefix) and row["path"].count("/") == 2}
        assert names == {"SYSTEM VOLUME INFORMATION", "DOCS", "DATA", "DEDUP",
                         "VOLUME_TAG.TXT", "TREE", "CHANGELOG.TXT"}
        if part == "part4":
            prefix = "disk1/part4/DOCS/"
            docs = {row["path"][len(prefix):].casefold(): row["size"] for row in rows
                    if row["path"].startswith(prefix)}
            expected = {f"doc_{index:02d}.txt": 2048 for index in range(20)}
            expected["h\u00e9llo-\u00fcnicode-\u00fcmlaut.txt"] = 29
            expected["long_name_" * 18 + ".txt"] = 14
            assert docs == expected


def test_acronis_mounted_fat32_table_sectors(peare_lib):
    path = Path(SAMPLES) / "t1_disk_mbr.tibx"
    if not path.is_file():
        pytest.skip("Independent TIBX FAT32 backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "b00c255734cfb3e74bfc07a5ad9ab61d89868ce2cb7a95fd3ef7df61d369b4ff"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert _resource_hash(opener, "disk1/part2", ranges=[(7038 * 512, 512),
                                                              ((7038 + 577) * 512, 512),
                                                              (7038 * 512, 577 * 512),
                                                              ((7038 + 577) * 512, 577 * 512)]) == [
            "b6ea6bfa16bb54e28800d76c2a6cb05e77fed2e0d98cf85f4e910b4cbc5cb870",
            "b6ea6bfa16bb54e28800d76c2a6cb05e77fed2e0d98cf85f4e910b4cbc5cb870",
            "2af76c22eb2cf4ea77eebfabb8ac3e73869f258af41906d5e9495e2cf65d5d6f",
            "2af76c22eb2cf4ea77eebfabb8ac3e73869f258af41906d5e9495e2cf65d5d6f",
        ]


def test_acronis_mounted_fat16_table_sectors(peare_lib):
    path = Path(SAMPLES) / "t1_disk_gpt.tibx"
    if not path.is_file():
        pytest.skip("Independent TIBX FAT16 backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "4023f69b9ec43a4cdc389b00b070a2da55334b36adc6c8dc27fc7d5b4ff549a5"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert _resource_hash(opener, "disk1/part4", ranges=[(8 * 512, 32 * 512),
                                                              (40 * 512, 32 * 512)]) == [
            "54232c920ac53c4e89b79bc92d3305d1c4ef88ff99d972f229c45004959de360",
            "54232c920ac53c4e89b79bc92d3305d1c4ef88ff99d972f229c45004959de360",
        ]


def test_sector_by_sector_unformatted_partition(peare_lib):
    path = Path(SAMPLES) / "t1_disk_gpt.tibx"
    if not path.is_file():
        pytest.skip("Independent TIBX sector-by-sector backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "4023f69b9ec43a4cdc389b00b070a2da55334b36adc6c8dc27fc7d5b4ff549a5"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert _resource_hash(opener, "disk1/part5") == (
            "bc39b2ea36fbf31d5b811d89b7686a34e09d6e44049c7cb4bab1f890b6070724"
        )


def test_acronis15_sector_by_sector_fat16(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "disk-raw.tibx"
    source_path = Path(SAMPLES) / "acronis15-created" / "disk-raw-source.vhdx"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 sector-by-sector backup not available")
    if not source_path.is_file():
        pytest.skip("Acronis Cyber Protect 15 source disk not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "7fc62232a8e6d75f1f88308ede39e79217e2a1fef4288cb08c47b41efd2289a4"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        with peare_lib.open_file(str(source_path)) as source:
            assert source is not None
            fat_ranges = [(2 * 512, 251 * 512), (253 * 512, 251 * 512)]
            assert _resource_hash(opener, "disk1/part1", ranges=fat_ranges) == (
                _resource_hash(source, "Whole disk", ranges=[
                    (128 * 512 + offset, length) for offset, length in fat_ranges
                ])
            )
        assert _resource_hash(opener, "disk1/part1") == (
            "ec7e1120318b272c9d5556eae108ca2d99552de7aab7c3d136e8a033abb99901"
        )
        assert _resource_hash(opener, "disk1/part1", ranges=[(2 * 512, 251 * 512),
                                                              (253 * 512, 251 * 512)]) == [
            "9edadf96442e32d45cabfdcc2666f0574fdce5c737c5100edd5f391a11985a89",
            "9edadf96442e32d45cabfdcc2666f0574fdce5c737c5100edd5f391a11985a89",
        ]


def test_acronis15_file_backup(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "modern.tibx"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 file backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "1ac1efcecb401fd4cb4c335a433a88ed0fc22fb36b4ff8d938a5e6edee958d71"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert opener.container_format() == "TIBX"
        rows = opener.normalized_tree(max_depth=4)
        assert len({row["path"] for row in rows}) == len(rows)
        item = next(row for row in rows if row["path"] == "C:/PeareInput/sample.txt")
        assert not item["is_container"]
        assert item["size"] == 23
        assert item["sha256"] == (
            "aac8714a07f6e40e95d7fa5ac6f2acd42417eb86b8f1cbd76076fc7726d7c62c"
        )


@pytest.mark.parametrize(("name", "digest", "password"), [
    ("modern-incremental.tibx", "887d6074bd64150b3e830ab7027d1e8ad5f1850f87434838fcd17e27dd67847d", None),
    ("modern-aes-incremental.tibx", "afe13b34af3086be03796970828cc1191064d6f8f12bdfe513868353c5235227", "PeareTest2026"),
])
def test_acronis15_incremental_file_backup(peare_lib, name, digest, password):
    path = Path(SAMPLES) / "acronis15-created" / name
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 incremental backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    with peare_lib.open_file(str(path), password) as opener:
        assert opener is not None
        rows = opener.normalized_tree(max_depth=5)
        by_path = {row["path"]: row for row in rows}
        assert len(by_path) == len(rows)
        assert by_path["2/C:/PeareInput/sample.txt"]["sha256"] == (
            "aac8714a07f6e40e95d7fa5ac6f2acd42417eb86b8f1cbd76076fc7726d7c62c"
        )
        assert by_path["3/C:/PeareInput/sample.txt"]["sha256"] == (
            "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff"
        )


def test_acronis15_many_file_backup(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "manyentries.tibx"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 many-file backup not available")
    archive = path.read_bytes()
    assert hashlib.sha256(archive).hexdigest() == (
        "c08922e71df09cef4d25ed5efceed529d627a404a0f5bbfe4e9ed3317c29a607"
    )
    pages = [archive[offset:offset + 4096] for offset in range(0, len(archive), 4096)]
    assert sum(page[1] == 3 and page[8:12] == b"LEAF" for page in pages) > 1
    assert any(page[1] == 4 and page[8:12] == b"LDIR" for page in pages)
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        rows = opener.normalized_tree(max_depth=5)
        assert len(rows) == len({row["path"] for row in rows})
        files = {row["path"]: row["sha256"] for row in rows if not row["is_container"]}
        expected = {
            "C:/PeareMany/group%02d/entry%03d.txt" % (group, item):
                hashlib.sha256(("Peare known entry %03d" % item).encode("ascii")).hexdigest()
            for group in range(8) for item in range(128)
        }
        assert files == expected


def test_acronis15_many_file_incremental_backup(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "manyentries-incremental.tibx"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 many-file incremental backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "3753bcf5d54504f3a54535fa319cc2d5694238f2ac09b2cecd2e19b507cfbb4a"
    )
    expected = {
        "C:/PeareMany/group%02d/entry%03d.txt" % (group, item):
            hashlib.sha256(("Peare known entry %03d" % item).encode("ascii")).hexdigest()
        for group in range(8) for item in range(128)
    }
    latest = dict(expected)
    for name in ("group00/entry000.txt", "group04/entry064.txt", "group07/entry127.txt"):
        del latest["C:/PeareMany/" + name]
    latest["C:/PeareMany/group03/entry042.txt"] = hashlib.sha256(b"Modified entry 042").hexdigest()
    latest["C:/PeareMany/group06/new-item.txt"] = hashlib.sha256(b"New item").hexdigest()
    latest["C:/PeareMany/group02/renamed012.txt"] = latest.pop("C:/PeareMany/group02/entry012.txt")
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        rows = opener.normalized_tree(max_depth=6)
        assert len(rows) == len({row["path"] for row in rows})
        files = {row["path"]: row["sha256"] for row in rows if not row["is_container"]}
        assert files == {**{"2/" + key: value for key, value in expected.items()},
                         **{"3/" + key: value for key, value in latest.items()}}


def test_acronis15_max_compression_file_backup(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "modern-max.tibx"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 compressed file backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "fbf3e6f814060983471c6629a09ddb3124fddd56ae2a1413a3f6188292139062"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert opener.container_format() == "TIBX"
        assert _resource_hash(opener, "C:/Windows/System32/ntdll.dll") == (
            "43a5a732667cad04776a1ccf51e152c48705ad3ba486e0c628b7386e9a7138d7"
        )


def test_acronis15_encrypted_file_backup(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "modern-aes.tibx"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 encrypted file backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "2df09cc40e5122dd96b771f2ffbd291333f7e84a9f5b6fae227caf132a3bbc09"
    )
    assert peare_lib.open_file(str(path)) is None
    assert peare_lib.last_open_status == 14
    assert peare_lib.open_file(str(path), "incorrect") is None
    assert peare_lib.last_open_status == 15
    root = peare_lib.open_file(str(path), "PeareTest2026")
    assert root is not None
    openers = [root]
    try:
        rows = root.normalized_tree(max_depth=4)
        item = next(row for row in rows if row["path"] == "C:/PeareInput/sample.txt")
        assert item["sha256"] == (
            "aac8714a07f6e40e95d7fa5ac6f2acd42417eb86b8f1cbd76076fc7726d7c62c"
        )
        current = root
        for name in ("C:", "PeareInput"):
            source = _directory_source(current, name)
            try:
                status, child = _open_source_with_password(peare_lib, source, "PeareTest2026")
            finally:
                current._L.peare_source_destroy(source)
            assert status == 0 and child is not None
            openers.append(child)
            current = child
        assert _resource_hash(current, "sample.txt") == (
            "aac8714a07f6e40e95d7fa5ac6f2acd42417eb86b8f1cbd76076fc7726d7c62c"
        )
    finally:
        for opener in reversed(openers):
            opener.close()


@pytest.mark.parametrize(("name", "digest"), [
    ("modern-aes128.tibx", "c92ca50ca09f474e6f782747823fd86f071d1faaa639f64ea2929ade5831dd8d"),
    ("modern-aes192.tibx", "303b1eeb49641fd85fd249e18f9bbcbea7115848cebe7f66ed4011456b40fc53"),
])
def test_acronis15_additional_aes_file_backup(peare_lib, name, digest):
    path = Path(SAMPLES) / "acronis15-created" / name
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 AES backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    assert peare_lib.open_file(str(path)) is None
    assert peare_lib.last_open_status == 14
    assert peare_lib.open_file(str(path), "incorrect") is None
    assert peare_lib.last_open_status == 15
    with peare_lib.open_file(str(path), "PeareTest2026") as root:
        rows = root.normalized_tree(max_depth=4)
        item = next(row for row in rows if row["path"] == "C:/PeareInput/sample.txt")
        assert item["size"] == 36
        assert item["sha256"] == "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff"


@pytest.fixture(params=[
    ("rsa-aes128.tibx", "ffdf0e557709fef5636a72492b69936dfdc78e84024faa38449300d7235da55e", 18),
    ("rsa-aes128-certificate-only.tibx", "6a69ce2864d2dda3acf506ee4c08e28cee5259882053a86aecafc5fdc75c2b88", 16),
], ids=["password-or-key", "certificate-only"])
def tibx_rsa_sample(request):
    directory = Path(SAMPLES) / "public-key-aes128-20261115"
    name, digest, required_status = request.param
    archive = directory / name
    key = directory / "test-private.pem"
    if not archive.is_file() or not key.is_file():
        pytest.skip("Acronis TIBX RSA sample and test private key not available")
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest
    return archive, key, required_status


def _set_tibx_private_key(lib, handle, key):
    setter = lib.peare_opener_set_tibx_private_key
    setter.argtypes = [_HANDLE, C.POINTER(C.c_uint8), C.c_size_t]
    setter.restype = C.c_int
    if key is None:
        return setter(handle, None, 0)
    buffer = (C.c_uint8 * len(key)).from_buffer_copy(key)
    status = setter(handle, buffer, len(key))
    C.memset(buffer, 0, len(key))
    return status


def test_tibx_rsa_private_key_and_reset(peare_lib, tibx_rsa_sample):
    archive, key_path, required_status = tibx_rsa_sample
    key = key_path.read_bytes()
    lib = peare_lib._lib
    handle = _HANDLE()
    assert lib.peare_opener_create(C.byref(handle)) == 0
    openers = []
    try:
        assert lib.peare_opener_open_file(handle, str(archive).encode("utf-8")) == required_status
        assert _set_tibx_private_key(lib, handle, key) == 0
        assert lib.peare_opener_open_file(handle, str(archive).encode("utf-8")) == 0
        root = Opener(peare_lib, handle)
        openers.append(root)
        assert root.container_format() == "TIBX"
        current = root
        for name in ("C:", "PeareInput"):
            source = _directory_source(current, name)
            child_handle = _HANDLE()
            try:
                assert lib.peare_opener_create(C.byref(child_handle)) == 0
                assert _set_tibx_private_key(lib, child_handle, key) == 0
                assert lib.peare_opener_open(child_handle, source) == 0
                child = Opener(peare_lib, child_handle)
                openers.append(child)
                child_handle = _HANDLE()
                current = child
            finally:
                if child_handle:
                    lib.peare_opener_destroy(child_handle)
                lib.peare_source_destroy(source)
        assert _resource_hash(current, "sample.txt") == (
            "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff"
        )
        for child in reversed(openers[1:]):
            child.close()
        openers = [root]
        lib.peare_opener_close.argtypes = [_HANDLE]
        lib.peare_opener_close.restype = C.c_int
        assert lib.peare_opener_close(handle) == 0
        assert _set_tibx_private_key(lib, handle, None) == 0
        assert lib.peare_opener_open_file(handle, str(archive).encode("utf-8")) == required_status
        assert _set_tibx_private_key(lib, handle, key) == 0
        assert lib.peare_opener_open_file(handle, str(archive).encode("utf-8")) == 0
    finally:
        if openers:
            for opener in reversed(openers):
                opener.close()
        else:
            lib.peare_opener_destroy(handle)


def test_tibx_rsa_wrong_private_key(peare_lib, tibx_rsa_sample):
    rsa = pytest.importorskip("cryptography.hazmat.primitives.asymmetric.rsa")
    serialization = pytest.importorskip("cryptography.hazmat.primitives.serialization")
    archive, _, _ = tibx_rsa_sample
    wrong = rsa.generate_private_key(public_exponent=65537, key_size=2048).private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    lib = peare_lib._lib
    handle = _HANDLE()
    assert lib.peare_opener_create(C.byref(handle)) == 0
    try:
        assert _set_tibx_private_key(lib, handle, wrong) == 0
        assert lib.peare_opener_open_file(handle, str(archive).encode("utf-8")) == 17
    finally:
        lib.peare_opener_destroy(handle)


def _set_tibx_key_passphrase(lib, handle, passphrase):
    setter = lib.peare_opener_set_tibx_key_passphrase
    setter.argtypes = [_HANDLE, C.c_char_p]
    setter.restype = C.c_int
    return setter(handle, None if passphrase is None else passphrase.encode("utf-8"))


def test_tibx_encrypted_pem_credentials(peare_lib, tibx_rsa_sample):
    serialization = pytest.importorskip("cryptography.hazmat.primitives.serialization")
    archive, key_path, required_status = tibx_rsa_sample
    private_key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    passphrase = "chiave-\u00e8"
    encrypted = private_key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.BestAvailableEncryption(passphrase.encode("utf-8")),
    )
    lib = peare_lib._lib
    handle = _HANDLE()
    assert lib.peare_opener_create(C.byref(handle)) == 0
    openers = []
    path = str(archive).encode("utf-8")
    try:
        assert _set_tibx_private_key(lib, handle, encrypted) == 0
        assert lib.peare_opener_open_file(handle, path) == 19
        if required_status == 18:
            assert lib.peare_opener_set_password(handle, b"PeareTest2026") == 0
            assert lib.peare_opener_open_file(handle, path) == 0
            lib.peare_opener_close.argtypes = [_HANDLE]
            lib.peare_opener_close.restype = C.c_int
            assert lib.peare_opener_close(handle) == 0
            assert _set_tibx_private_key(lib, handle, encrypted) == 0
        assert _set_tibx_key_passphrase(lib, handle, "wrong") == 0
        assert lib.peare_opener_open_file(handle, path) == 20
        if required_status == 18:
            assert lib.peare_opener_set_password(handle, b"PeareTest2026") == 0
            assert lib.peare_opener_open_file(handle, path) == 0
            lib.peare_opener_close.argtypes = [_HANDLE]
            lib.peare_opener_close.restype = C.c_int
            assert lib.peare_opener_close(handle) == 0
            assert _set_tibx_private_key(lib, handle, encrypted) == 0
        assert _set_tibx_key_passphrase(lib, handle, passphrase) == 0
        assert lib.peare_opener_open_file(handle, path) == 0
        root = Opener(peare_lib, handle)
        openers.append(root)
        current = root
        for name in ("C:", "PeareInput"):
            source = _directory_source(current, name)
            child_handle = _HANDLE()
            try:
                assert lib.peare_opener_create(C.byref(child_handle)) == 0
                assert _set_tibx_private_key(lib, child_handle, encrypted) == 0
                assert _set_tibx_key_passphrase(lib, child_handle, passphrase) == 0
                assert lib.peare_opener_open(child_handle, source) == 0
                current = Opener(peare_lib, child_handle)
                openers.append(current)
                child_handle = _HANDLE()
            finally:
                if child_handle:
                    lib.peare_opener_destroy(child_handle)
                lib.peare_source_destroy(source)
        assert _resource_hash(current, "sample.txt") == (
            "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff"
        )
        for child in reversed(openers[1:]):
            child.close()
        openers = [root]
        lib.peare_opener_close.argtypes = [_HANDLE]
        lib.peare_opener_close.restype = C.c_int
        assert lib.peare_opener_close(handle) == 0
        assert _set_tibx_private_key(lib, handle, encrypted) == 0
        assert lib.peare_opener_open_file(handle, path) == 19
        assert _set_tibx_key_passphrase(lib, handle, "") == 0
        assert lib.peare_opener_open_file(handle, path) == 20
        assert _set_tibx_key_passphrase(lib, handle, None) == 0
        assert lib.peare_opener_open_file(handle, path) == 19
    finally:
        if openers:
            for opener in reversed(openers):
                opener.close()
        else:
            lib.peare_opener_destroy(handle)


@pytest.mark.parametrize("tibx_rsa_sample", [
    ("rsa-aes128-certificate-only.tibx", "6a69ce2864d2dda3acf506ee4c08e28cee5259882053a86aecafc5fdc75c2b88", 16),
], indirect=True)
def test_tibx_encrypted_pem_empty_passphrase(peare_lib, tibx_rsa_sample):
    openssl = shutil.which("openssl")
    if not openssl:
        pytest.skip("OpenSSL CLI is required to generate an empty-passphrase PEM")
    archive, key_path, _ = tibx_rsa_sample
    generated = subprocess.run(
        [openssl, "pkcs8", "-topk8", "-v2", "aes-256-cbc", "-iter", "1000", "-passout", "pass:"],
        input=key_path.read_bytes(), capture_output=True, check=True,
    )
    lib = peare_lib._lib
    handle = _HANDLE()
    assert lib.peare_opener_create(C.byref(handle)) == 0
    try:
        assert _set_tibx_private_key(lib, handle, generated.stdout) == 0
        assert lib.peare_opener_open_file(handle, str(archive).encode("utf-8")) == 19
        assert _set_tibx_key_passphrase(lib, handle, "") == 0
        assert lib.peare_opener_open_file(handle, str(archive).encode("utf-8")) == 0
    finally:
        lib.peare_opener_destroy(handle)


@pytest.mark.parametrize("tibx_rsa_sample", [
    ("rsa-aes128-certificate-only.tibx", "6a69ce2864d2dda3acf506ee4c08e28cee5259882053a86aecafc5fdc75c2b88", 16),
], indirect=True)
def test_tibx_encrypted_pem_reasonable_work(peare_lib, tibx_rsa_sample):
    openssl = shutil.which("openssl")
    if not openssl:
        pytest.skip("OpenSSL CLI is required to generate an encrypted PEM")
    archive, key_path, _ = tibx_rsa_sample
    generated = subprocess.run(
        [openssl, "pkcs8", "-topk8", "-v2", "aes-256-cbc", "-v2prf", "hmacWithSHA256",
         "-iter", "100000", "-passout", "pass:test-passphrase"],
        input=key_path.read_bytes(), capture_output=True, check=True,
    )
    lib = peare_lib._lib
    handle = _HANDLE()
    assert lib.peare_opener_create(C.byref(handle)) == 0
    try:
        assert _set_tibx_private_key(lib, handle, generated.stdout) == 0
        assert _set_tibx_key_passphrase(lib, handle, "test-passphrase") == 0
        assert lib.peare_opener_open_file(handle, str(archive).encode("utf-8")) == 0
    finally:
        lib.peare_opener_destroy(handle)


@pytest.mark.parametrize(("iterations", "sha1"), [
    (1000001, False), (500001, True), (18446744073709551615, False),
])
def test_tibx_encrypted_pem_kdf_work_limit(peare_lib, tibx_rsa_sample, iterations, sha1):
    serialization = pytest.importorskip("cryptography.hazmat.primitives.serialization")
    decoder = pytest.importorskip("pyasn1.codec.der.decoder")
    encoder = pytest.importorskip("pyasn1.codec.der.encoder")
    archive, key_path, required_status = tibx_rsa_sample
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    encrypted = key.private_bytes(
        serialization.Encoding.DER, serialization.PrivateFormat.PKCS8,
        serialization.BestAvailableEncryption(b"test-passphrase"),
    )
    envelope, remainder = decoder.decode(encrypted)
    assert not remainder
    params = envelope[0][1][0][1]
    params[1] = iterations
    if sha1:
        params[2][0] = "1.2.840.113549.2.7"
    encoded = base64.b64encode(encoder.encode(envelope))
    pem = b"-----BEGIN ENCRYPTED PRIVATE KEY-----\n" + encoded + b"\n-----END ENCRYPTED PRIVATE KEY-----\n"

    qt_dir = ""
    if os.name == "nt":
        kernel = C.windll.kernel32
        kernel.GetModuleHandleW.argtypes = [C.c_wchar_p]
        kernel.GetModuleHandleW.restype = C.c_void_p
        kernel.GetModuleFileNameW.argtypes = [C.c_void_p, C.c_wchar_p, C.c_uint32]
        kernel.GetModuleFileNameW.restype = C.c_uint32
        for name in ("Qt5Core.dll", "Qt6Core.dll"):
            module = kernel.GetModuleHandleW(name)
            if module:
                buf = C.create_unicode_buffer(32768)
                assert kernel.GetModuleFileNameW(module, buf, len(buf))
                qt_dir = str(Path(buf.value).parent)
                break
    worker = r'''
import ctypes as C, os, sys
from pathlib import Path
libpath, archive, qt_dir, use_password = sys.argv[1:]
dirs = []
if hasattr(os, "add_dll_directory"):
    dirs.append(os.add_dll_directory(str(Path(libpath).parent)))
    if qt_dir:
        dirs.append(os.add_dll_directory(qt_dir))
lib = C.CDLL(libpath)
h = C.c_void_p()
lib.peare_opener_create.argtypes = [C.POINTER(C.c_void_p)]
lib.peare_opener_destroy.argtypes = [C.c_void_p]
lib.peare_opener_set_tibx_private_key.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
lib.peare_opener_set_tibx_key_passphrase.argtypes = [C.c_void_p, C.c_char_p]
lib.peare_opener_set_password.argtypes = [C.c_void_p, C.c_char_p]
lib.peare_opener_open_file.argtypes = [C.c_void_p, C.c_char_p]
pem = sys.stdin.buffer.read()
assert lib.peare_opener_create(C.byref(h)) == 0
try:
    buf = C.create_string_buffer(pem)
    assert lib.peare_opener_set_tibx_private_key(h, buf, len(pem)) == 0
    assert lib.peare_opener_set_tibx_key_passphrase(h, b'test-passphrase') == 0
    if use_password == 'yes':
        assert lib.peare_opener_set_password(h, b'PeareTest2026') == 0
    print(lib.peare_opener_open_file(h, archive.encode('utf-8')))
finally:
    lib.peare_opener_destroy(h)
'''
    def open_in_worker(password):
        result = subprocess.run(
            [sys.executable, "-c", worker, str(peare_lib._lib._name), str(archive), qt_dir, password],
            input=pem, capture_output=True, timeout=20, check=True,
        )
        return int(result.stdout.strip())

    assert open_in_worker("no") == 21
    assert "limit" in peare_lib.status_message(21).lower()
    if required_status == 18:
        assert open_in_worker("yes") == 0


def _rsa_pkcs8_variant(key_pem, variant):
    serialization = pytest.importorskip("cryptography.hazmat.primitives.serialization")
    rsa = pytest.importorskip("cryptography.hazmat.primitives.asymmetric.rsa")
    decoder = pytest.importorskip("pyasn1.codec.der.decoder")
    encoder = pytest.importorskip("pyasn1.codec.der.encoder")
    namedtype = pytest.importorskip("pyasn1.type.namedtype")
    tag = pytest.importorskip("pyasn1.type.tag")
    univ = pytest.importorskip("pyasn1.type.univ")
    char = pytest.importorskip("pyasn1.type.char")

    private_key = serialization.load_pem_private_key(key_pem, password=None)
    original = private_key.private_bytes(
        serialization.Encoding.DER, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )

    class PrivateKeyInfo(univ.Sequence):
        componentType = namedtype.NamedTypes(
            namedtype.NamedType("version", univ.Integer()),
            namedtype.NamedType("algorithm", univ.Any()),
            namedtype.NamedType("privateKey", univ.OctetString()),
            namedtype.OptionalNamedType("attributes", univ.SetOf(componentType=univ.Any()).subtype(
                implicitTag=tag.Tag(tag.tagClassContext, tag.tagFormatConstructed, 0))),
            namedtype.OptionalNamedType("publicKey", univ.BitString().subtype(
                implicitTag=tag.Tag(tag.tagClassContext, tag.tagFormatSimple, 1))),
        )

    info, remainder = decoder.decode(original, asn1Spec=PrivateKeyInfo())
    assert not remainder
    if variant == "attributes":
        attribute = univ.Sequence()
        attribute.setComponentByPosition(0, univ.ObjectIdentifier("1.2.3.4"))
        values = univ.SetOf(componentType=char.UTF8String())
        values.setComponentByPosition(0, char.UTF8String("Peare test"))
        attribute.setComponentByPosition(1, values)
        attributes = info.getComponentByName("attributes").clone()
        attributes.setComponentByPosition(0, univ.Any(encoder.encode(attribute)))
        info.setComponentByName("attributes", attributes)
    else:
        info.setComponentByName("version", 1)
        public_key = (rsa.generate_private_key(public_exponent=65537, key_size=2048).public_key()
                      if variant == "mismatched-public-key" else private_key.public_key())
        public_der = public_key.public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.PKCS1,
        )
        info.setComponentByName("publicKey", univ.BitString.fromOctetString(public_der).subtype(
            implicitTag=tag.Tag(tag.tagClassContext, tag.tagFormatSimple, 1)))
    encoded = base64.b64encode(encoder.encode(info))
    lines = [encoded[i:i + 64] for i in range(0, len(encoded), 64)]
    return b"-----BEGIN PRIVATE KEY-----\n" + b"\n".join(lines) + b"\n-----END PRIVATE KEY-----\n"


@pytest.mark.parametrize("variant", [
    "pem-preamble-crlf", "attributes", "public-key", "mismatched-public-key",
])
@pytest.mark.parametrize("tibx_rsa_sample", [
    ("rsa-aes128-certificate-only.tibx", "6a69ce2864d2dda3acf506ee4c08e28cee5259882053a86aecafc5fdc75c2b88", 16),
], indirect=True)
def test_tibx_rsa_private_key_encodings(peare_lib, tibx_rsa_sample, variant):
    archive, key_path, _ = tibx_rsa_sample
    key = key_path.read_bytes()
    if variant == "pem-preamble-crlf":
        key = b"Bag Attributes\r\n    friendlyName: Peare test\r\n" + key.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    else:
        key = _rsa_pkcs8_variant(key, variant)
    lib = peare_lib._lib
    handle = _HANDLE()
    assert lib.peare_opener_create(C.byref(handle)) == 0
    try:
        assert _set_tibx_private_key(lib, handle, key) == 0
        status = lib.peare_opener_open_file(handle, str(archive).encode("utf-8"))
        assert status == (17 if variant == "mismatched-public-key" else 0)
    finally:
        lib.peare_opener_destroy(handle)


@pytest.mark.parametrize("valid_key", [False, True])
def test_tibx_rsa_independent_credentials(peare_lib, tibx_rsa_sample, valid_key):
    rsa = pytest.importorskip("cryptography.hazmat.primitives.asymmetric.rsa")
    serialization = pytest.importorskip("cryptography.hazmat.primitives.serialization")
    archive, key_path, required_status = tibx_rsa_sample
    wrong_key = rsa.generate_private_key(public_exponent=65537, key_size=2048).private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    lib = peare_lib._lib
    handle = _HANDLE()
    assert lib.peare_opener_create(C.byref(handle)) == 0
    try:
        assert _set_tibx_private_key(lib, handle, key_path.read_bytes() if valid_key else wrong_key) == 0
        password = b"incorrect" if valid_key else b"PeareTest2026"
        assert lib.peare_opener_set_password(handle, password) == 0
        status = lib.peare_opener_open_file(handle, str(archive).encode("utf-8"))
        assert status == (17 if not valid_key and required_status == 16 else 0)
    finally:
        lib.peare_opener_destroy(handle)


def test_tibx_rsa_password_still_works(peare_lib, tibx_rsa_sample):
    archive, _, required_status = tibx_rsa_sample
    opener = peare_lib.open_file(str(archive), "PeareTest2026")
    if required_status == 16:
        if opener is not None:
            opener.close()
        assert opener is None
        assert peare_lib.last_open_status == 16
        return
    with opener:
        assert opener is not None
        assert _resource_hash(opener, "C:/PeareInput/sample.txt", "PeareTest2026") == (
            "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff"
        )


def test_tibx_large_read_can_continue_after_short_result(peare_lib):
    path = Path(SAMPLES) / "t1_simple_ntfs.tibx"
    with peare_lib.open_file(str(path)) as root:
        source = _directory_source(root, "disk1")
        try:
            status, directory = _open_source_with_password(peare_lib, source, None)
        finally:
            root._L.peare_source_destroy(source)
        assert status == 0 and directory is not None
        resource = _HANDLE()
        try:
            find = directory._L.peare_opener_find_resource
            find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
            find.restype = C.c_int
            assert find(directory._h, b"TIBX_VOLUME", b"part1", None, C.byref(resource)) == 0
            context = _Context()
            assert directory._L.peare_resource_get_context(resource, C.byref(context)) == 0
            try:
                assert context.data_size >= 2 * 1024 * 1024
            finally:
                directory._L.peare_resource_context_free(C.byref(context))
            target = 2 * 1024 * 1024
            chunks = []
            while sum(map(len, chunks)) < target:
                offset = sum(map(len, chunks))
                chunk = peare_lib.read_resource(resource, offset, target - offset)
                assert chunk
                chunks.append(chunk)
            result = b"".join(chunks)
            assert len(result) == target
            expected = b"".join(peare_lib.read_resource(resource, offset, 65536)
                                for offset in range(0, target, 65536))
            assert result == expected
        finally:
            if resource:
                directory._L.peare_resource_destroy(resource)
            directory.close()


def _resource_hash(opener, path, password=None, ranges=None, check_eof=None, original=False):
    first, separator, remaining = path.partition("/")
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(resource)) == 0
            try:
                context = _Context()
                assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
                name = context.identifier_utf8.to_bytes().decode("utf-8")
                size = int(context.data_size)
                container = bool(context.is_container)
                opener._L.peare_resource_context_free(C.byref(context))
                if name != first:
                    continue
                if separator:
                    assert container, path
                    source = _HANDLE()
                    child_handle = _HANDLE()
                    assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
                    assert opener._L.peare_opener_create(C.byref(child_handle)) == 0
                    try:
                        if password is not None:
                            assert opener._L.peare_opener_set_password(child_handle, password.encode("utf-8")) == 0
                        assert opener._L.peare_opener_open(child_handle, source) == 0
                        child = Opener(opener._lib, child_handle)
                        try:
                            return _resource_hash(child, remaining, password, ranges, check_eof, original)
                        finally:
                            child.close()
                            child_handle = _HANDLE()
                    finally:
                        if child_handle:
                            opener._L.peare_opener_destroy(child_handle)
                        opener._L.peare_source_destroy(source)
                read_resource = opener._lib.read_resource_original if original else opener._lib.read_resource
                if ranges is not None:
                    hashes = []
                    for offset, length in ranges:
                        assert offset + length <= size
                        data = read_resource(resource, offset, length)
                        assert len(data) == length
                        hashes.append(hashlib.sha256(data).hexdigest())
                    return hashes
                if check_eof is not None:
                    expected_size, last_byte = check_eof
                    assert size == expected_size
                    assert opener._lib.read_resource(resource, size - 1, 8) == last_byte
                    assert opener._lib.read_resource(resource, size, 8) == b""
                    buffer = (C.c_uint8 * 8)()
                    count = C.c_size_t()
                    assert opener._L.peare_resource_read(
                        resource, size + 1, buffer, 8, C.byref(count)
                    ) == 5
                    assert count.value == 0
                digest = hashlib.sha256()
                offset = 0
                while offset < size:
                    data = read_resource(resource, offset, min(1024 * 1024, size - offset))
                    assert data
                    repeat = read_resource(resource, offset, min(64, len(data)))
                    assert repeat == data[:len(repeat)] and repeat
                    digest.update(data)
                    offset += len(data)
                return digest.hexdigest()
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"TIBX resource not found: {path}")


def test_tibx_real_linked_alias_is_not_listed_twice(peare_lib):
    path = Path(SAMPLES) / "t1_simple_ntfs.tibx"
    expected = "2f37a8c006003a77723338f740e61ecec223095ab43852c132e10d1af3829fa0"
    with peare_lib.open_file(str(path)) as opener:
        assert _resource_hash(opener, "disk1/part1") == expected
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
        assert "part" not in names


@pytest.mark.parametrize(("sample", "path", "expected_hash", "eof"), [
    ("testAcronis_full_b1_s1_v1.tib", "C:/testAcronis/100MB.bin:Zone.Identifier:$DATA",
     "f870e553a2c6289e1ee41c5d06eb49c0ba2efcdeeee0dccfc37dc589da561bd3", (109, b"\n")),
    ("t1_legacy_tib.tibx", "H:/docs/doc_00.txt",
     "c9c981f0ae0f298bdfdf638a4e83361bce220eff307b6db95713fff27b7999ba", (2048, b"e")),
])
def test_acronis_nested_resource_eof(peare_lib, sample, path, expected_hash, eof):
    with peare_lib.open_file(str(Path(SAMPLES) / sample)) as opener:
        assert _resource_hash(opener, path, check_eof=eof) == expected_hash


@pytest.mark.parametrize(("sample", "path", "expected"), [
    ("t1_simple_ntfs.tibx", "disk1/part1", "2f37a8c006003a77723338f740e61ecec223095ab43852c132e10d1af3829fa0"),
    ("t1_empty_ntfs.tibx", "disk1/part1", "1794acc080aef2a8e725a3b7413439d8ed0f4d4101bb8300fd8bbe3dbed980f8"),
    ("t1_multivol.tibx", "disk1/part1", "1061b9a1f37adf51541022e37bd135b3b035cb4509c5535dcdeed16df59064f3"),
    ("t1_multivol.tibx", "disk1/part2", "e9eb847d8679fc46524113c35e690cb9d453e635cd3892cfa28ef3c02c89aeb5"),
    ("t1_multivol.tibx", "disk1/part3", "66f749e041b978167f28b3296aa16e5f9cd737b847a63c26c3902241deb137d6"),
    ("t1_stored.tibx", "disk1/part1", "acf750441e2c47f8fcc7448e3bfd15bf99c6818cd2c737ae81f1a5ca0143d0d6"),
    ("t1_disk_mbr.tibx", "disk1/part1", "c4dbdfcbf81928cb0e835af1f880e17da4314d4f946bc5fe9ac5ae82c8fb4b46"),
    ("t1_disk_mbr.tibx", "disk1/part2", "ee814888c3afbceb93b3ca903f4a59ddbc94527ad6b2dfedc1c111e8fb29b60d"),
    ("t1_chain_inc.tibx", "2/disk1/part1", "14601f43adb0681ddf24a630acc050d00d1448627d5d0e810f925786efb5b8fb"),
    ("t1_chain_inc.tibx", "3/disk1/part1", "ed9dfc11d43d84d2a88acfb094ca80cd6859966cfafd2d740a22c4b42225025a"),
    ("t1_chain_inc.tibx", "4/disk1/part1", "e7e893d56c2ca997fbf4c86864927bcdf3b498205f46ada0fd4c3d6f2488f4df"),
    ("t1_chain_diff.tibx", "2/disk1/part1", "f6b4233798da6b68af87db396f3cd2ff843be0a4b8f63fdd0ad03d67ac6ef5e3"),
    ("t1_chain_diff.tibx", "4/disk1/part1", "8c808b51732b3f380b388a88593728a8077c906e528d8fdfa3a0cc4bc0c50745"),
    ("t1_chain_diff-0001.tibx", "4/disk1/part1", "8c808b51732b3f380b388a88593728a8077c906e528d8fdfa3a0cc4bc0c50745"),
    ("ati_2020_disk_lz4_compr.tibx", "5/disk1/part1", "d29989304fc63657dfdab3fd7fb0a804a747b93580b7abb622a29f3d55ae33e7"),
    ("ati_2020_disk_lz4_compr.tibx", "2/disk1/part1", "5647f05ec18958947d32874eeb788fa396a05d0bab7c1b71f112ceb7e9b31eee"),
    ("ati_2020_disk_lz4_compr.tibx", "3/disk1/part1", "5647f05ec18958947d32874eeb788fa396a05d0bab7c1b71f112ceb7e9b31eee"),
    ("ati_2020_disk_lz4_compr.tibx", "4/disk1/part1", "b69c98b5ac590d9173bd06df0388cbdd18ad0a12acf5963bcd6607254ac8ff36"),
    ("t1_disk_gpt.tibx", "disk1/part1", "5f5332ec4a0ef1e632ba208db29658935556b0b7e8a3b55523e0b65ce06aa886"),
    ("t1_disk_gpt.tibx", "disk1/part2", "e9eb847d8679fc46524113c35e690cb9d453e635cd3892cfa28ef3c02c89aeb5"),
    ("t1_disk_gpt.tibx", "disk1/part3", "66f749e041b978167f28b3296aa16e5f9cd737b847a63c26c3902241deb137d6"),
    ("t1_disk_gpt.tibx", "disk1/part4", "d8e6fdf30c6d1980cb645ea5bec3b11b1fd2c961f56991f1bfa7332e7531f5e9"),
    ("t1_disk_gpt.tibx", "disk1/part5", "bc39b2ea36fbf31d5b811d89b7686a34e09d6e44049c7cb4bab1f890b6070724"),
    ("t1_disk_gpt.tibx", "disk1/part6", "763290e445bbccdec4635f93cc1e8dd0dd9845b2c2d85f46a1956544d2d736ee"),
    ("t1_disk_gpt.tibx", "disk1/part7", "3f89b9aa78a6b51912e061dcd1971682779afdc1a61ad3363bd95b24f2fa9049"),
    ("t1_disk_gpt.tibx", "disk1/part8", "18fdb2a2fbfda6a212c8c82d848835dc863e905a481ad400c0ba0c084d80fce7"),
    ("t1_legacy_tib.tibx", "H:/docs/doc_00.txt", "c9c981f0ae0f298bdfdf638a4e83361bce220eff307b6db95713fff27b7999ba"),
    ("t1_legacy_tib.tibx", "H:/docs/h\u00e9llo-\u00fcnicode-\u00fcmlaut.txt", "aa914484b7ed6500f809532f160288872e2d64c632f0f6dc8065e86b74322c7f"),
])
def test_tibx_volume_bytes(peare_lib, sample, path, expected):
    with peare_lib.open_file(str(Path(SAMPLES) / sample)) as opener:
        assert opener.container_format() == "TIBX"
        assert _resource_hash(opener, path, original=True) == expected


def test_tibx_nested_ntfs(peare_lib):
    with peare_lib.open_file(str(Path(SAMPLES) / "t1_simple_ntfs.tibx")) as opener:
        paths = [row["path"] for row in opener.normalized_tree(max_depth=2)]
        assert len(paths) == len(set(paths))
        assert "disk1/part1/changelog.txt" in paths


def test_tibx_interleaved_volume_reads(peare_lib):
    with peare_lib.open_file(str(Path(SAMPLES) / "t1_disk_gpt.tibx")) as opener:
        resource = _HANDLE()
        source = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            context = _Context()
            assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
            name = context.identifier_utf8.to_bytes().decode("utf-8")
            opener._L.peare_resource_context_free(C.byref(context))
            assert name == "disk1"
            assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
            with peare_lib._open_source(source) as disk:
                first = "5f5332ec4a0ef1e632ba208db29658935556b0b7e8a3b55523e0b65ce06aa886"
                second = "e9eb847d8679fc46524113c35e690cb9d453e635cd3892cfa28ef3c02c89aeb5"
                assert _resource_hash(disk, "part1") == first
                assert _resource_hash(disk, "part2", original=True) == second
                assert _resource_hash(disk, "part1") == first
        finally:
            if source:
                opener._L.peare_source_destroy(source)
            opener._L.peare_resource_destroy(resource)


def test_tibx_concurrent_interleaved_volume_reads(peare_lib):
    path = str(Path(SAMPLES) / "t1_multivol.tibx")
    expected = [
        ("part1", "1061b9a1f37adf51541022e37bd135b3b035cb4509c5535dcdeed16df59064f3"),
        ("part2", "e9eb847d8679fc46524113c35e690cb9d453e635cd3892cfa28ef3c02c89aeb5"),
    ]
    with peare_lib.open_file(path) as opener:
        directory = _HANDLE()
        source = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(directory)) == 0
        try:
            context = _Context()
            assert opener._L.peare_resource_get_context(directory, C.byref(context)) == 0
            try:
                assert context.identifier_utf8.to_bytes().decode("utf-8") == "disk1"
            finally:
                opener._L.peare_resource_context_free(C.byref(context))
            assert opener._L.peare_resource_get_source(directory, C.byref(source)) == 0
            with peare_lib._open_source(source) as disk:
                resources = []
                references = []
                try:
                    for index, (name, expected_hash) in enumerate(expected):
                        resource = _HANDLE()
                        assert disk._L.peare_opener_open_resource_at(disk._h, 0, index, C.byref(resource)) == 0
                        resources.append(resource)
                        context = _Context()
                        assert disk._L.peare_resource_get_context(resource, C.byref(context)) == 0
                        try:
                            assert context.identifier_utf8.to_bytes().decode("utf-8") == name
                            size = int(context.data_size)
                        finally:
                            disk._L.peare_resource_context_free(C.byref(context))
                        reference = bytearray()
                        for offset in range(0, size, 1024 * 1024):
                            count = min(1024 * 1024, size - offset)
                            data = peare_lib.read_resource_original(resource, offset, count)
                            assert len(data) == count
                            reference.extend(data)
                        assert hashlib.sha256(reference).hexdigest() == expected_hash
                        references.append(reference)

                    barrier = threading.Barrier(4)

                    def read_interleaved(worker):
                        rng = random.Random(worker)
                        barrier.wait(timeout=30)
                        for index in range(200):
                            volume = (worker + index) % 2
                            offset = rng.randrange(len(references[volume]) - 4096)
                            actual = peare_lib.read_resource_original(resources[volume], offset, 4096)
                            assert actual == references[volume][offset:offset + 4096], (worker, index, volume, offset)

                    with ThreadPoolExecutor(max_workers=4) as pool:
                        futures = [pool.submit(read_interleaved, worker) for worker in range(4)]
                        for future in futures:
                            future.result()
                finally:
                    for resource in resources:
                        disk._L.peare_resource_destroy(resource)
        finally:
            if source:
                opener._L.peare_source_destroy(source)
            opener._L.peare_resource_destroy(directory)


def test_tibx_segment_cache_concurrent_reads(peare_lib):
    path = str(Path(SAMPLES) / "t1_stored.tibx")
    expected_hash = "acf750441e2c47f8fcc7448e3bfd15bf99c6818cd2c737ae81f1a5ca0143d0d6"
    with peare_lib.open_file(path) as opener:
        directory = _HANDLE()
        source = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(directory)) == 0
        try:
            assert opener._L.peare_resource_get_source(directory, C.byref(source)) == 0
            with peare_lib._open_source(source) as disk:
                resource = _HANDLE()
                assert disk._L.peare_opener_open_resource_at(disk._h, 0, 0, C.byref(resource)) == 0
                try:
                    context = _Context()
                    assert disk._L.peare_resource_get_context(resource, C.byref(context)) == 0
                    try:
                        assert context.identifier_utf8.to_bytes().decode("utf-8") == "part1"
                        size = int(context.data_size)
                    finally:
                        disk._L.peare_resource_context_free(C.byref(context))

                    reference = bytearray()
                    for offset in range(0, size, 1024 * 1024):
                        count = min(1024 * 1024, size - offset)
                        data = peare_lib.read_resource(resource, offset, count)
                        assert len(data) == count
                        reference.extend(data)
                    assert hashlib.sha256(reference).hexdigest() == expected_hash

                    chunk_size = 64 * 1024
                    workers = 4
                    barrier = threading.Barrier(workers)

                    def read_chunks(worker):
                        barrier.wait(timeout=30)
                        offsets = list(range(worker * chunk_size, size, workers * chunk_size))
                        if worker % 2:
                            offsets.reverse()
                        for offset in offsets:
                            count = min(chunk_size, size - offset)
                            actual = peare_lib.read_resource(resource, offset, count)
                            assert actual == reference[offset:offset + count], offset
                        for _ in range(32):
                            for offset in (0, size // 4, size // 2, 15466496):
                                count = min(257, size - offset)
                                assert count > 0
                                actual = peare_lib.read_resource(resource, offset, count)
                                assert actual == reference[offset:offset + count], offset

                    with ThreadPoolExecutor(max_workers=workers) as pool:
                        futures = [pool.submit(read_chunks, worker) for worker in range(workers)]
                        for future in futures:
                            future.result()
                finally:
                    disk._L.peare_resource_destroy(resource)
        finally:
            if source:
                opener._L.peare_source_destroy(source)
            opener._L.peare_resource_destroy(directory)


@pytest.mark.parametrize(("sample", "path", "expected", "password"), [
    ("acronis15-created/diskmodern.tibx", "disk1/part1/DATA/CHANGE.TXT", "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765", None),
    ("t1_simple_ntfs.tibx", "disk1/part1/changelog.txt", "f34848ca92665c342abd5816c9e3eda0e82180671195362bcd0080544a3bc2ac", None),
    ("t1_legacy_tib.tibx", "H:/docs/h\u00e9llo-\u00fcnicode-\u00fcmlaut.txt", "aa914484b7ed6500f809532f160288872e2d64c632f0f6dc8065e86b74322c7f", None),
    ("t1_enc_aes.tibx", "disk1/part1/changelog.txt", "f34848ca92665c342abd5816c9e3eda0e82180671195362bcd0080544a3bc2ac", "dissect"),
    ("ati_2020_disk_enc_lz4_compr.tibx", "3/disk1/part1/README.txt.txt", "42419493599cb9ec560610adb33ec701c511e80b5212036ad83475b3648d165c", "dissect"),
    ("t1_chain_inc.tibx", "3/disk1/part1/ntfs/lznt1_compressed.txt", "8f4c07a3841244b053f89d11c0a63d124c9cfc2811de36dea64f7ae6c4422d01", None),
    ("acronis15-created/modern-aes128.tibx", "C:/PeareInput/sample.txt", "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff", "PeareTest2026"),
    ("acronis15-created/modern-aes192.tibx", "C:/PeareInput/sample.txt", "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff", "PeareTest2026"),
])
def test_tibx_gui_nested_preview(peare_gui_path, tmp_path, sample, path, expected, password):
    if sample.startswith("acronis15-created/modern-aes") and not (Path(SAMPLES) / sample).is_file():
        pytest.skip("The independent Acronis AES sample is not available")
    root = Path(__file__).resolve().parents[1]
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    if password is not None:
        environment["PEARE_PREVIEW_PASSWORD"] = password
    else:
        environment.pop("PEARE_PREVIEW_PASSWORD", None)
    plugin_name = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    configured = environment.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    candidates = ([Path(configured)] if configured else []) + [
        root / "build-windows" / "test-plugins" / "platforms", peare_gui_path.parent / "platforms"]
    plugin_dir = next((item for item in candidates if (item / plugin_name).is_file()), None)
    assert plugin_dir is not None, "Offscreen Qt plugin is missing"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    output = tmp_path / "preview.png"
    result = subprocess.run([
        str(peare_gui_path), "--preview-resource", str(Path(SAMPLES) / sample), path, str(output),
    ], capture_output=True, text=True, env=environment, timeout=120, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"] and inspection["sha256"] == expected
    assert not inspection["duplicate_paths"]
    assert output.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"


def test_tibx_all_file_backup_contents(peare_lib):
    with peare_lib.open_file(str(Path(SAMPLES) / "t1_legacy_tib.tibx")) as opener:
        root = opener.normalized_tree(max_depth=0, recurse_containers=False)
        assert [row["path"] for row in root] == ["H:"]
        assert root[0]["is_container"]
        rows = opener.normalized_tree(max_depth=3, include_resource_type=True)
        paths = [row["path"] for row in rows if row["resource_type"] == "TIBX_FILE"]
        assert len(paths) == len(set(paths)) == 22
        digest = hashlib.sha256()
        for path in sorted(paths):
            digest.update(path.encode("utf-8") + b"\0" + bytes.fromhex(_resource_hash(opener, path)))
        assert digest.hexdigest() == "14d0422f06ada131e3be8214379564d7e7e899624cbb162c044b304c7f24ac6c"


def test_tibx_encrypted_volume(peare_lib):
    path = str(Path(SAMPLES) / "t1_enc_aes.tibx")
    assert peare_lib.open_file(path) is None
    assert peare_lib.last_open_status == 14
    assert peare_lib.open_file(path, "wrong") is None
    assert peare_lib.last_open_status == 15
    with peare_lib.open_file(path, "dissect") as opener:
        assert _resource_hash(opener, "disk1/part1", "dissect") == "d13f328efd801ac02dfb847a83bb1c0432182273619cf3b7121558f2bb177ea0"
        assert _resource_hash(opener, "disk1/part1/changelog.txt", "dissect") == "f34848ca92665c342abd5816c9e3eda0e82180671195362bcd0080544a3bc2ac"


def _directory_source(opener, directory):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(resource)) == 0
            try:
                context = _Context()
                assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
                try:
                    name = context.identifier_utf8.to_bytes().decode("utf-8")
                    is_container = bool(context.is_container)
                finally:
                    opener._L.peare_resource_context_free(C.byref(context))
                if name == directory:
                    assert is_container
                    source = _HANDLE()
                    assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
                    return source
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"TIBX directory not found: {directory}")


def _open_source_with_password(peare_lib, source, password):
    handle = _HANDLE()
    assert peare_lib._lib.peare_opener_create(C.byref(handle)) == 0
    try:
        if password is not None:
            assert peare_lib._lib.peare_opener_set_password(handle, password.encode("utf-8")) == 0
        status = peare_lib._lib.peare_opener_open(handle, source)
        if status != 0:
            return status, None
        opener = Opener(peare_lib, handle)
        handle = _HANDLE()
        return status, opener
    finally:
        if handle:
            peare_lib._lib.peare_opener_destroy(handle)


def test_tibx_incremental_ntfs_lznt1(peare_lib):
    root = peare_lib.open_file(str(Path(SAMPLES) / "t1_chain_inc.tibx"))
    assert root is not None
    openers = [root]
    try:
        current = root
        for name in ("3", "disk1", "part1", "ntfs"):
            source = _directory_source(current, name)
            try:
                status, child = _open_source_with_password(peare_lib, source, None)
            finally:
                peare_lib._lib.peare_source_destroy(source)
            assert status == 0 and child is not None
            openers.append(child)
            current = child
        found = False
        for folder in range(current.folder_count()):
            for index in range(current.resource_count(folder)):
                resource = _HANDLE()
                assert current._L.peare_opener_open_resource_at(current._h, folder, index, C.byref(resource)) == 0
                try:
                    context = _Context()
                    assert current._L.peare_resource_get_context(resource, C.byref(context)) == 0
                    try:
                        name = context.identifier_utf8.to_bytes().decode("utf-8")
                        size = int(context.data_size)
                    finally:
                        current._L.peare_resource_context_free(C.byref(context))
                    if name != "lznt1_compressed.txt":
                        continue
                    assert not found
                    found = True
                    assert size == 16384
                    reference = peare_lib.read_resource(resource, 0, size)
                    assert len(reference) == size
                    assert hashlib.sha256(reference).hexdigest() == (
                        "8f4c07a3841244b053f89d11c0a63d124c9cfc2811de36dea64f7ae6c4422d01"
                    )
                    def check_reads(worker):
                        for i in range(100):
                            offset = (worker * 997 + i * 251) % size
                            count = min(173, size - offset)
                            assert peare_lib.read_resource(resource, offset, count) == reference[offset:offset + count]
                        for offset in (0, 4095, 4096, 8191, 8192, size - 1):
                            assert peare_lib.read_resource(resource, offset, 1) == reference[offset:offset + 1]
                    with ThreadPoolExecutor(max_workers=4) as pool:
                        list(pool.map(check_reads, range(4)))
                finally:
                    current._L.peare_resource_destroy(resource)
        assert found
    finally:
        for opener in reversed(openers):
            opener.close()


def test_tibx_incremental_ntfs_file_size_uses_data_stream(peare_lib):
    path = str(Path(SAMPLES) / "t1_chain_inc.tibx")
    root = peare_lib.open_file(path)
    assert root is not None
    openers = [root]
    try:
        current = root
        for name in ("3", "disk1", "part1", "System Volume Information"):
            source = _directory_source(current, name)
            try:
                status, child = _open_source_with_password(peare_lib, source, None)
            finally:
                peare_lib._lib.peare_source_destroy(source)
            assert status == 0 and child is not None
            openers.append(child)
            current = child
        found = False
        for folder in range(current.folder_count()):
            for index in range(current.resource_count(folder)):
                resource = _HANDLE()
                assert current._L.peare_opener_open_resource_at(current._h, folder, index, C.byref(resource)) == 0
                try:
                    context = _Context()
                    assert current._L.peare_resource_get_context(resource, C.byref(context)) == 0
                    try:
                        name = context.identifier_utf8.to_bytes().decode("utf-8")
                        size = int(context.data_size)
                    finally:
                        current._L.peare_resource_context_free(C.byref(context))
                    if name != "MasterFileStatus.db":
                        continue
                    assert not found
                    found = True
                    assert size == 22464
                    data = peare_lib.read_resource(resource, 0, size)
                    assert len(data) == size
                    assert hashlib.sha256(data).hexdigest() == (
                        "9e36ef9f46ead13e7359a7e56cc6e9bc64b90bccba9c46906e0be40b465dee2e"
                    )
                finally:
                    current._L.peare_resource_destroy(resource)
        assert found
    finally:
        for opener in reversed(openers):
            opener.close()


def test_tibx_encrypted_directory_source_survives_parent(peare_lib):
    path = str(Path(SAMPLES) / "t1_enc_aes.tibx")
    parent = peare_lib.open_file(path, "dissect")
    assert parent is not None
    try:
        source = _directory_source(parent, "disk1")
    finally:
        parent.close()
    try:
        status, child = _open_source_with_password(peare_lib, source, "wrong")
        if child is not None:
            child.close()
        assert status == 15 and child is None
        status, child = _open_source_with_password(peare_lib, source, None)
        if child is not None:
            child.close()
        assert status == 14 and child is None
        status, child = _open_source_with_password(peare_lib, source, "dissect")
        assert status == 0 and child is not None
        with child:
            assert _resource_hash(child, "part1") == "d13f328efd801ac02dfb847a83bb1c0432182273619cf3b7121558f2bb177ea0"
    finally:
        peare_lib._lib.peare_source_destroy(source)


def test_tibx_encrypted_independent_openers_keep_passwords_isolated(peare_lib):
    path = str(Path(SAMPLES) / "t1_enc_aes.tibx")
    first = peare_lib.open_file(path, "dissect")
    assert first is not None
    second = peare_lib.open_file(path, "dissect")
    if second is None:
        first.close()
    assert second is not None
    sources = []
    try:
        sources.append(_directory_source(first, "disk1"))
        sources.append(_directory_source(second, "disk1"))
        first.close()
        second.close()
        wrong = peare_lib.open_file(path, "wrong")
        if wrong is not None:
            wrong.close()
        assert wrong is None
        assert peare_lib.last_open_status == 15
        for source in sources:
            status, child = _open_source_with_password(peare_lib, source, "wrong")
            if child is not None:
                child.close()
            assert status == 15 and child is None
            status, child = _open_source_with_password(peare_lib, source, "dissect")
            assert status == 0 and child is not None
            with child:
                assert _resource_hash(child, "part1/changelog.txt", "dissect") == "f34848ca92665c342abd5816c9e3eda0e82180671195362bcd0080544a3bc2ac"
    finally:
        first.close()
        second.close()
        for source in sources:
            peare_lib._lib.peare_source_destroy(source)


def test_tibx_encrypted_incremental_cbc(peare_lib):
    path = str(Path(SAMPLES) / "ati_2020_disk_enc_lz4_compr.tibx")
    assert peare_lib.open_file(path) is None
    assert peare_lib.last_open_status == 14
    assert peare_lib.open_file(path, "wrong") is None
    assert peare_lib.last_open_status == 15
    with peare_lib.open_file(path, "dissect") as opener:
        assert _resource_hash(opener, "2/disk1/part1", "dissect") == "0a2702360cdb9902aa64f59f072402736b4f0f57be4fc1e83a5884900e226084"
        assert _resource_hash(opener, "3/disk1/part1", "dissect") == "122fd1031eec87f138c2c8607fb858bb81ec3d9041115344a5b8a52e1f2251c0"


def test_tibx_encrypted_incremental_overlapping_extents(peare_lib):
    path = str(Path(SAMPLES) / "ati_2020_disk_enc_lz4_compr.tibx")
    with peare_lib.open_file(path, "dissect") as opener:
        assert _resource_hash(opener, "3/disk1/part1", "dissect", [(1044480, 4096), (2637824, 4096)]) == [
            "21f252db745fe94cc375950f12cb84bca7f66b3ea92b564204dfc0b86da31057",
            "d8ddf12195d2d3131c612ab5e3f25c3ac458b00b72c2a8e36e03e6dfd0c9f77c",
        ]


def test_tibx_missing_continuation_fails(peare_lib, tmp_path):
    source = Path(SAMPLES) / "t1_chain_diff.tibx"
    isolated = tmp_path / source.name
    shutil.copyfile(source, isolated)
    assert peare_lib.open_file(str(isolated)) is None


@pytest.mark.parametrize("selected", ["t1_chain_diff.tibx", "t1_chain_diff-0001.tibx"])
def test_tibx_real_split_selected_part(peare_lib, selected):
    with peare_lib.open_file(str(Path(SAMPLES) / selected)) as opener:
        assert _resource_hash(opener, "4/disk1/part1") == (
            "8c808b51732b3f380b388a88593728a8077c906e528d8fdfa3a0cc4bc0c50745"
        )


@pytest.mark.parametrize("selected", ["Win10-2020-11-28.tibx", "Win10-2020-11-28-0006.tibx"])
def test_tibx_real_win10_split_selected_part(peare_lib, selected):
    directory = os.environ.get("PEARE_TIBX_WIN10_SPLIT_DIR")
    if not directory:
        pytest.skip("Set PEARE_TIBX_WIN10_SPLIT_DIR to the original seven-part archive")
    expected = {
        14577303552: "a383e68c206d212d07e2ee2284a26e878b118db04cc7a430689f18dee7119600",
        838860800: "8c81cfe9ac42dd234ab2ab7bad3de61e7acdc3332a235d7c54166dcff2d1f754",
        104857600: "91d42c418f31cb92c92095662f06fe1eb19391289690fdd39aada0998edc771f",
    }
    found = {}

    def visit(current, depth):
        assert depth <= 3
        for folder in range(current.folder_count()):
            for index in range(current.resource_count(folder)):
                resource = _HANDLE()
                assert current._L.peare_opener_open_resource_at(current._h, folder, index, C.byref(resource)) == 0
                try:
                    context = _Context()
                    assert current._L.peare_resource_get_context(resource, C.byref(context)) == 0
                    size, container = int(context.data_size), bool(context.is_container)
                    current._L.peare_resource_context_free(C.byref(context))
                    if size in expected:
                        found[size] = hashlib.sha256(current._lib.read_resource(resource, 0, 4096)).hexdigest()
                    elif container:
                        source = _HANDLE()
                        assert current._L.peare_resource_get_source(resource, C.byref(source)) == 0
                        try:
                            child = peare_lib._open_source(source)
                            assert child is not None
                            with child:
                                visit(child, depth + 1)
                        finally:
                            current._L.peare_source_destroy(source)
                finally:
                    current._L.peare_resource_destroy(resource)

    with peare_lib.open_file(str(Path(directory) / selected)) as opener:
        visit(opener, 0)
    assert found == expected


def test_tibx_unrelated_continuation_fails(peare_lib, tmp_path):
    base = tmp_path / "t1_chain_diff.tibx"
    continuation = tmp_path / "t1_chain_diff-0001.tibx"
    shutil.copyfile(Path(SAMPLES) / base.name, base)
    shutil.copyfile(Path(SAMPLES) / "t1_stored.tibx", continuation)
    assert peare_lib.open_file(str(base)) is None


def test_tibx_missing_numbered_part_with_unrelated_later_part_fails(peare_lib, tmp_path):
    base = tmp_path / "t1_chain_diff.tibx"
    shutil.copyfile(Path(SAMPLES) / base.name, base)
    shutil.copyfile(Path(SAMPLES) / "t1_stored.tibx", tmp_path / "t1_chain_diff-0002.tibx")
    assert peare_lib.open_file(str(base)) is None


def test_tibx_unrelated_intermediate_part_fails(peare_lib, tmp_path):
    base = tmp_path / "t1_chain_diff.tibx"
    shutil.copyfile(Path(SAMPLES) / base.name, base)
    shutil.copyfile(Path(SAMPLES) / "t1_stored.tibx", tmp_path / "t1_chain_diff-0001.tibx")
    shutil.copyfile(Path(SAMPLES) / "t1_chain_diff-0001.tibx", tmp_path / "t1_chain_diff-0002.tibx")
    assert peare_lib.open_file(str(base)) is None


def test_tibx_standalone_ignores_unrelated_numbered_file(peare_lib, tmp_path):
    base = tmp_path / "archive.tibx"
    unrelated = tmp_path / "archive-0001.tibx"
    shutil.copyfile(Path(SAMPLES) / "t1_simple_ntfs.tibx", base)
    shutil.copyfile(Path(SAMPLES) / "t1_stored.tibx", unrelated)
    shutil.copyfile(Path(SAMPLES) / "t1_stored.tibx", tmp_path / "archive-0000.tibx")
    with peare_lib.open_file(str(base)) as opener:
        assert _resource_hash(opener, "disk1/part1") == "2f37a8c006003a77723338f740e61ecec223095ab43852c132e10d1af3829fa0"


def test_tibx_standalone_ignores_unrelated_continuation(peare_lib, tmp_path):
    base = tmp_path / "archive.tibx"
    unrelated = tmp_path / "archive-0001.tibx"
    shutil.copyfile(Path(SAMPLES) / "t1_stored.tibx", base)
    shutil.copyfile(Path(SAMPLES) / "t1_chain_diff-0001.tibx", unrelated)
    with peare_lib.open_file(str(base)) as opener:
        assert _resource_hash(opener, "disk1/part1") == "acf750441e2c47f8fcc7448e3bfd15bf99c6818cd2c737ae81f1a5ca0143d0d6"
