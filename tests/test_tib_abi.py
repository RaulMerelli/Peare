import ctypes as C
import hashlib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import threading
import zlib
from contextlib import ExitStack
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from peare_opener import _Context, _HANDLE


SAMPLES = os.environ.get("PEARE_ACRONIS_SAMPLES")
pytestmark = pytest.mark.skipif(not SAMPLES, reason="Set PEARE_ACRONIS_SAMPLES to the independent sample directory")


def test_trueimage8_filesystem_v1_catalog(peare_lib):
    sample = Path(SAMPLES) / "trueimage8-en-764-demo.tib"
    if not sample.is_file():
        pytest.skip("True Image 8.0.764 filesystem-mode sample not available")
    assert hashlib.sha256(sample.read_bytes()).hexdigest() == (
        "ee73dac71b7de477fe7d6211950618190c2c4988230dd261dd4f9a925e5d817c"
    )
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(include_resource_type=True)
    disks = [row for row in rows if row["resource_type"] == "TIB_8F_DISK_RECORD"]
    partitions = [row for row in rows if row["resource_type"] == "TIB_8F_PARTITION_RECORD"]
    assert len(disks) == 2, rows
    assert len(partitions) == 5, rows
    assert all(row["size"] > 2 and not row["is_container"] for row in rows), rows


def test_trueimage9_2273_file_level_sample(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "ti9-2273-file-level.tib"
    if not sample.is_file():
        pytest.skip("True Image 9.0.2273 file-level sample not available")
    expected = {
        "C:/WINDOWS/system32/drivers/etc/services": (7228, "0624427750d6c5304ef4f1e53a88dfd0c5fb28301d9b2a891475335799cdd4d6"),
        "C:/WINDOWS/system32/drivers/etc/protocol": (847, "73fbcd77513cc384a8206d8ca12d7e6b13de29b1ad8cdab9da28515fbc69886c"),
        "C:/WINDOWS/system32/drivers/etc/networks": (394, "0862a9fbadea3bf76d41055f64f9d0bd859c914a50f76ff8e272f614311c3838"),
        "C:/WINDOWS/system32/drivers/etc/lmhosts.sam": (3784, "028ad8169b8f7a7805e304c65ff106057ef773e2517820841691bafce43aad69"),
        "C:/WINDOWS/system32/drivers/etc/hosts": (768, "16c35dcbec2baedb0e24851a960b9262806532ffac5c8049918beb253aa2c49c"),
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=8)
        files = {row["path"]: row for row in rows if row["path"] in expected}
        assert set(files) == set(expected), rows
        for path, (size, digest) in expected.items():
            assert files[path]["size"] == size
            assert _resource_hash(opener, path, "pearetest2026") == digest


def test_trueimage9_3677_file_level_sample(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "ti9-3677-system-config.tib"
    if not sample.is_file():
        pytest.skip("True Image 9.0.3677 file-level sample not available")
    expected = {
        "C:\\/WINDOWS/system32/drivers/etc/services": (7228, "0624427750d6c5304ef4f1e53a88dfd0c5fb28301d9b2a891475335799cdd4d6"),
        "C:\\/WINDOWS/system32/drivers/etc/protocol": (847, "73fbcd77513cc384a8206d8ca12d7e6b13de29b1ad8cdab9da28515fbc69886c"),
        "C:\\/WINDOWS/system32/drivers/etc/networks": (394, "0862a9fbadea3bf76d41055f64f9d0bd859c914a50f76ff8e272f614311c3838"),
        "C:\\/WINDOWS/system32/drivers/etc/lmhosts.sam": (3784, "028ad8169b8f7a7805e304c65ff106057ef773e2517820841691bafce43aad69"),
        "C:\\/WINDOWS/system32/drivers/etc/hosts": (768, "16c35dcbec2baedb0e24851a960b9262806532ffac5c8049918beb253aa2c49c"),
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=8)
        files = {row["path"]: row for row in rows if row["path"] in expected}
        assert set(files) == set(expected), rows
        for path, (size, digest) in expected.items():
            assert files[path]["size"] == size
            assert _resource_hash(opener, path) == digest


def test_home2009_9769_file_level_sample(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "home2009-9769-file-level-20261123" / "home2009-files.tib"
    if not sample.is_file():
        pytest.skip("True Image Home 2009 file-level sample not available")
    assert hashlib.sha256(sample.read_bytes()).hexdigest() == (
        "1d5651a0daa67a087aff44f146a71589940ec44e2d77916382bdd8b8ce04a545"
    )
    expected = {
        "C:/test/new.txt": (92, "bbe4c81d6be58f26caffab988c3a3379c676727c34d0b1c92fe1ad9d666f5595"),
        "C:/test/test.txt": (92, "bbe4c81d6be58f26caffab988c3a3379c676727c34d0b1c92fe1ad9d666f5595"),
        "C:/test/sub/cmd.exe": (398336, "31b6f35592ba702170351bae60fff2cfd00a5120a60f8d73d28638ceae223047"),
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=8)
        files = {row["path"]: row for row in rows if row["path"] in expected}
        assert set(files) == set(expected), rows
        for path, (size, digest) in expected.items():
            assert files[path]["size"] == size
            assert _resource_hash(opener, path) == digest


@pytest.mark.parametrize(("filename", "archive_sha256"), [
    ("PeareAES128.tib", "9d2968de81e4ea15fd62d2e5c3d0f422098cc44b48dfe6003dc0618eee364468"),
    ("PeareAES192.tib", "007d2c5c3066f3c0bc39b38fb9b865dec8877b4ceff9750d0d6918ba976805de"),
    ("PeareAES256.tib", "4d38f5a7f3fdae4eda4c86e1d569bbe228b5bc67218965760115d962d7823bfd"),
])
def test_home2009_9769_encrypted_file_level_samples(peare_lib, filename, archive_sha256):
    from test_tibx_abi import _resource_hash

    sample = (Path(SAMPLES) / "home2009-9769-file-level-encrypted-20261123" / filename)
    if not sample.is_file():
        pytest.skip("True Image Home 2009 encrypted file-level samples not available")
    assert hashlib.sha256(sample.read_bytes()).hexdigest() == archive_sha256
    opener = peare_lib.open_file(str(sample), "pearetest2026")
    assert opener is not None
    with opener:
        rows = {row["path"]: row for row in opener.normalized_tree(max_depth=8)}
        expected = {
            "C:/test/new.txt": (92, "bbe4c81d6be58f26caffab988c3a3379c676727c34d0b1c92fe1ad9d666f5595"),
            "C:/test/test.txt": (92, "bbe4c81d6be58f26caffab988c3a3379c676727c34d0b1c92fe1ad9d666f5595"),
            "C:/test/sub/cmd.exe": (398336, "31b6f35592ba702170351bae60fff2cfd00a5120a60f8d73d28638ceae223047"),
        }
        for path, (size, digest) in expected.items():
            assert path in rows, rows
            assert rows[path]["size"] == size
            assert _resource_hash(opener, path, "pearetest2026") == digest


def test_trueimage11_file_backup(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "trueimage11-file-sample.tib"
    if not sample.is_file():
        pytest.skip("True Image 11 file backup not available")
    expected = (b"Peare historical file-backup sample.\n"
                b"This file is included to verify the recovered payload.\n")
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=10)
        paths = [row["path"] for row in rows if row["path"].endswith("PEARE-TEST.TXT")]
        assert len(paths) == 1, rows
        assert _resource_hash(opener, paths[0]) == hashlib.sha256(expected).hexdigest()


def test_trueimage11_file_tree(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "trueimage11-tree.tib"
    if not sample.is_file():
        pytest.skip("True Image 11 directory backup not available")
    expected = {
        "empty.txt": hashlib.sha256(b"").hexdigest(),
        "test.txt": hashlib.sha256(b"Peare historical file-backup sample.\n"
                                   b"This file is included to verify the recovered payload.\n").hexdigest(),
        "cmd.exe": "31b6f35592ba702170351bae60fff2cfd00a5120a60f8d73d28638ceae223047",
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=10)
        assert len([row for row in rows if "/cmd.exe/" not in row["path"].lower()]) == 6, rows
        for name, digest in expected.items():
            matches = [row["path"] for row in rows if row["path"].rsplit("/", 1)[-1].lower() == name]
            assert len(matches) == 1, (name, rows)
            assert _resource_hash(opener, matches[0]) == digest


def test_trueimage11_incremental_file_tree(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "trueimage11-chain" / "tree2.tib"
    if not sample.is_file():
        pytest.skip("True Image 11 incremental backup not available")
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=10)
        paths = [row["path"] for row in rows]
        assert len([path for path in paths if "/cmd.exe/" not in path.lower()]) == 6, rows
        assert not any(path.lower().endswith("empty.txt") for path in paths)
        added = [path for path in paths if path.lower().endswith("new.txt")]
        assert len(added) == 1, rows
        expected = (b"Peare historical file-backup sample.\n"
                    b"This file is included to verify the recovered payload.\n")
        assert _resource_hash(opener, added[0]) == hashlib.sha256(expected).hexdigest()
        old = [path for path in paths if path.lower().endswith("cmd.exe")]
        assert len(old) == 1, rows
        assert _resource_hash(opener, old[0]) == "31b6f35592ba702170351bae60fff2cfd00a5120a60f8d73d28638ceae223047"


def test_trueimage11_incremental_base_snapshot(peare_lib):
    sample = Path(SAMPLES) / "trueimage11-chain" / "tree.tib"
    if not sample.is_file():
        pytest.skip("True Image 11 incremental backup not available")
    with peare_lib.open_file(str(sample)) as opener:
        paths = [row["path"].lower() for row in opener.normalized_tree(max_depth=10)]
        assert any(path.endswith("empty.txt") for path in paths)
        assert not any(path.endswith("new.txt") for path in paths)


def test_trueimage11_incremental_explicit_parts(peare_lib):
    from test_callback_source_abi import CallbackSource, _open_parts
    from test_tibx_abi import _resource_hash

    directory = Path(SAMPLES) / "trueimage11-chain"
    paths = [directory / "tree.tib", directory / "tree2.tib"]
    if not all(path.is_file() for path in paths):
        pytest.skip("True Image 11 incremental backup not available")
    sources = [CallbackSource(peare_lib, path) for path in paths]
    try:
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0 and opener is not None
        with opener:
            rows = opener.normalized_tree(max_depth=10)
            names = [row["path"] for row in rows]
            assert not any(name.lower().endswith("empty.txt") for name in names)
            new = [name for name in names if name.lower().endswith("new.txt")]
            assert len(new) == 1, rows
            expected = (b"Peare historical file-backup sample.\n"
                        b"This file is included to verify the recovered payload.\n")
            assert _resource_hash(opener, new[0]) == hashlib.sha256(expected).hexdigest()
    finally:
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)


@pytest.mark.parametrize("part", [1, 2, 3])
def test_trueimage11_split_file_backup(peare_lib, part):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "trueimage11-split" / ("MyBackup%d.tib" % part)
    if not sample.is_file():
        pytest.skip("True Image 11 split backup not available")
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=10)
        paths = [row["path"] for row in rows]
        for name in ("new.txt", "test.txt", "cmd.exe"):
            assert len([path for path in paths if path.lower().endswith(name)]) == 1, rows
        binary = next(path for path in paths if path.lower().endswith("cmd.exe"))
        assert _resource_hash(opener, binary) == "31b6f35592ba702170351bae60fff2cfd00a5120a60f8d73d28638ceae223047"


def test_trueimage11_split_missing_middle(peare_lib, tmp_path):
    directory = Path(SAMPLES) / "trueimage11-split"
    first, last = directory / "MyBackup1.tib", directory / "MyBackup3.tib"
    if not first.is_file() or not last.is_file():
        pytest.skip("True Image 11 split backup not available")
    shutil.copyfile(first, tmp_path / first.name)
    shutil.copyfile(last, tmp_path / last.name)
    assert peare_lib.open_file(str(tmp_path / first.name)) is None


def test_trueimage11_legacy_file_backup_payload(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "trueimage11-protected.tib"
    if not sample.is_file():
        pytest.skip("True Image 11 legacy file backup not available")
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        rows = opener.normalized_tree(max_depth=10)
        paths = [row["path"] for row in rows if row["path"].lower().endswith("cmd.exe")]
        assert len(paths) == 1, rows
        assert _resource_hash(opener, paths[0]) == "31b6f35592ba702170351bae60fff2cfd00a5120a60f8d73d28638ceae223047"


def test_trueimage11_incremental_missing_base(peare_lib, tmp_path):
    sample = Path(SAMPLES) / "trueimage11-chain" / "tree2.tib"
    if not sample.is_file():
        pytest.skip("True Image 11 incremental backup not available")
    orphan = tmp_path / sample.name
    shutil.copyfile(sample, orphan)
    assert peare_lib.open_file(str(orphan)) is None


@pytest.mark.parametrize("offset", [28, 412])
def test_trueimage11_file_backup_rejects_bad_checksum(peare_lib, tmp_path, offset):
    sample = Path(SAMPLES) / "trueimage11-file-sample.tib"
    if not sample.is_file():
        pytest.skip("True Image 11 file backup not available")
    data = bytearray(sample.read_bytes())
    data[offset] ^= 1
    damaged = tmp_path / "damaged.tib"
    damaged.write_bytes(data)
    assert peare_lib.open_file(str(damaged)) is None


def test_acronis15_forced_d4_disk_backup(peare_lib):
    from test_tibx_abi import _resource_hash

    vault = Path(SAMPLES) / "acronis15-created" / "d4-forced-20261117" / "Vault"
    sample = vault / "d4-forced.tib"
    if not sample.is_file():
        pytest.skip("Acronis D4 sample and external store not available")
    expected = {
        "alpha.bin": "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743",
        "same.bin": "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743",
        "variant.bin": "3a854bdbe1a78f1b6ed33072510c83ce8929f8b4417b6cecb9f564949d854366",
        "readme.txt": "2ae23fc848095781970d15b5abae83b57f1b5cff576bc891e3c1e423cbb1013c",
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=10)
        for name, digest in expected.items():
            paths = [r["path"] for r in rows if r["path"].rsplit("/", 1)[-1] == name]
            assert len(paths) == 1, (name, rows)
            assert _resource_hash(opener, paths[0]) == digest


def test_acronis15_external_dedup_store(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "acronis15-created" / "dedup-20261116" / "Vault" / "sample.tib"
    if not sample.is_file():
        pytest.skip("Acronis deduplicated sample not available")
    expected = {
        "alpha.bin": "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743",
        "same.bin": "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743",
        "variant.bin": "3a854bdbe1a78f1b6ed33072510c83ce8929f8b4417b6cecb9f564949d854366",
        "readme.txt": "2ae23fc848095781970d15b5abae83b57f1b5cff576bc891e3c1e423cbb1013c",
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=10)
        for name, digest in expected.items():
            matches = [r["path"] for r in rows if r["path"].rsplit("/", 1)[-1] == name]
            assert len(matches) == 1, (name, rows)
            assert _resource_hash(opener, matches[0]) == digest


def test_acronis15_external_dedup_compressed(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = (Path(SAMPLES) / "acronis15-created" / "dedup-compressed-20261116" /
              "Vault" / "compressed-sample.tib")
    if not sample.is_file():
        pytest.skip("Acronis compressed deduplicated sample not available")
    with peare_lib.open_file(str(sample)) as opener:
        assert _resource_hash(opener, "C:/PeareDedup/CompressInput/pattern.bin") == (
            "1e075c8d478ad21844e33e830a695ef03a4d2488b69ee275bd8947618bb1be1e"
        )


@pytest.mark.parametrize("password,valid", [
    ("PeareDedupTest2026!", True), ("wrong-password", False), (None, False),
])
def test_acronis15_external_dedup_encrypted(peare_lib, password, valid):
    from test_tibx_abi import _resource_hash

    sample = (Path(SAMPLES) / "acronis15-created" / "dedup-encrypted-20261116" /
              "Vault" / "encrypted-sample.tib")
    if not sample.is_file():
        pytest.skip("Acronis encrypted deduplicated sample not available")
    opener = peare_lib.open_file(str(sample), password=password)
    if valid:
        assert opener is not None
        with opener:
            expected = {
                "alpha.bin": "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743",
                "same.bin": "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743",
                "variant.bin": "3a854bdbe1a78f1b6ed33072510c83ce8929f8b4417b6cecb9f564949d854366",
                "readme.txt": "2ae23fc848095781970d15b5abae83b57f1b5cff576bc891e3c1e423cbb1013c",
            }
            for name, digest in expected.items():
                assert _resource_hash(opener, "C:/PeareDedup/Input/" + name) == digest
    else:
        assert opener is None
        assert peare_lib.last_open_status == (14 if password is None else 15)


def test_acronis15_external_dedup_encrypted_compressed(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = (Path(SAMPLES) / "acronis15-created" / "dedup-encrypted-compressed-20261116" /
              "Vault" / "compressed-sample.tib")
    if not sample.is_file():
        pytest.skip("Acronis encrypted compressed deduplicated sample not available")
    with peare_lib.open_file(str(sample), password="PeareDedupTest2026!") as opener:
        assert _resource_hash(opener, "C:/PeareDedup/CompressInput/pattern.bin") == (
            "1e075c8d478ad21844e33e830a695ef03a4d2488b69ee275bd8947618bb1be1e"
        )


@pytest.mark.parametrize("damage", ["footer_offset", "footer_offset_high", "trailer_magic", "padding"])
def test_acronis15_external_dedup_encrypted_bad_archive(peare_lib, tmp_path, damage):
    vault = (Path(SAMPLES) / "acronis15-created" / "dedup-encrypted-20261116" / "Vault")
    sample = vault / "encrypted-sample.tib"
    if not sample.is_file():
        pytest.skip("Acronis encrypted deduplicated sample not available")
    shutil.copytree(vault / ".meta", tmp_path / ".meta")
    size_file = vault / "unified_data.ds.0.szh"
    used = int.from_bytes(size_file.read_bytes(), "little")
    shutil.copyfile(size_file, tmp_path / size_file.name)
    with (vault / "unified_data.ds.0").open("rb") as src:
        (tmp_path / "unified_data.ds.0").write_bytes(src.read(used))
    data = bytearray(sample.read_bytes())
    trailer = int.from_bytes(data[-40:-36], "little") - 4
    assert trailer >= 48
    if damage == "footer_offset":
        data[-40:-36] = bytes(4)
    elif damage == "footer_offset_high":
        data[-36:-32] = b"\x01\x00\x00\x00"
    elif damage == "trailer_magic":
        data[trailer + 32] ^= 1
    else:
        data[trailer] = 16
    (tmp_path / "sample.tib").write_bytes(data)
    assert peare_lib.open_file(str(tmp_path / "sample.tib"), password="PeareDedupTest2026!") is None


@pytest.mark.parametrize("damage", ["corrupt", "truncated"])
def test_acronis15_external_dedup_encrypted_damaged(peare_lib, tmp_path, damage):
    from test_tibx_abi import _resource_hash

    vault = (Path(SAMPLES) / "acronis15-created" / "dedup-encrypted-20261116" / "Vault")
    sample = vault / "encrypted-sample.tib"
    if not sample.is_file():
        pytest.skip("Acronis encrypted deduplicated sample not available")
    shutil.copyfile(sample, tmp_path / "sample.tib")
    shutil.copytree(vault / ".meta", tmp_path / ".meta")
    length_file = vault / "unified_data.ds.0.szh"
    used = int.from_bytes(length_file.read_bytes(), "little")
    shutil.copyfile(length_file, tmp_path / length_file.name)
    with (vault / "unified_data.ds.0").open("rb") as src:
        data = bytearray(src.read(used))
    if damage == "corrupt":
        end = used
        while end > 4:
            stored = int.from_bytes(data[end - 16:end - 8], "little")
            start = end - 41 - stored
            assert start >= 4 and stored >= 52
            data[start + 32] ^= 1
            end = start
    else:
        del data[-1:]
    (tmp_path / "unified_data.ds.0").write_bytes(data)
    opener = peare_lib.open_file(str(tmp_path / "sample.tib"), password="PeareDedupTest2026!")
    assert opener is not None
    with opener:
        with pytest.raises((AssertionError, RuntimeError)):
            _resource_hash(opener, "C:/PeareDedup/Input/alpha.bin")


@pytest.mark.parametrize("variant", ["missing", "truncated", "corrupt", "zlib", "second_store"])
def test_acronis15_external_dedup_store_integrity(peare_lib, tmp_path, variant):
    from test_tibx_abi import _resource_hash

    vault = Path(SAMPLES) / "acronis15-created" / "dedup-20261116" / "Vault"
    sample = vault / "sample.tib"
    if not sample.is_file():
        pytest.skip("Acronis deduplicated sample not available")
    shutil.copyfile(sample, tmp_path / "sample.tib")
    size = int.from_bytes((vault / "unified_data.ds.0.szh").read_bytes(), "little")
    with (vault / "unified_data.ds.0").open("rb") as src:
        data = bytearray(src.read(size))
    if variant != "missing":
        if variant == "truncated":
            data = data[:-1]
        elif variant == "corrupt":
            data[4] ^= 1
        elif variant == "zlib":
            records = []
            end = len(data)
            while end > 4:
                footer = data[end - 33:end]
                raw, stored = struct.unpack_from("<QQ", footer, 17)
                start = end - 33 - stored
                assert footer[16] == 0 and raw == stored and start >= 4
                payload = zlib.compress(data[start:end - 33])
                records.append(payload + footer[:16] + b"\x01" + struct.pack("<QQ", raw, len(payload)))
                end = start
            assert end == 4
            data = data[:4] + b"".join(reversed(records))
            size = len(data)
        store_name = "unified_data.ds.1" if variant == "second_store" else "unified_data.ds.0"
        (tmp_path / store_name).write_bytes(data)
        (tmp_path / (store_name + ".szh")).write_bytes(struct.pack("<Q", size))
    with peare_lib.open_file(str(tmp_path / "sample.tib")) as opener:
        if variant in ("zlib", "second_store"):
            assert _resource_hash(opener, "C:/PeareDedup/Input/alpha.bin") == (
                "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743"
            )
        else:
            with pytest.raises((AssertionError, RuntimeError)):
                _resource_hash(opener, "C:/PeareDedup/Input/alpha.bin")


@pytest.mark.parametrize("store_name", ["unified_data.ds.0", "file_data.ds.0", "file_data.ds.1"])
@pytest.mark.parametrize("with_size_file", [True, False])
def test_acronis15_external_dedup_store_ancestor(peare_lib, tmp_path, store_name, with_size_file):
    from test_tibx_abi import _resource_hash

    vault = Path(SAMPLES) / "acronis15-created" / "dedup-20261116" / "Vault"
    sample = vault / "sample.tib"
    if not sample.is_file():
        pytest.skip("Acronis deduplicated sample not available")
    target = tmp_path / "Vault"
    archive_dir = target / "computers" / "machine" / "users" / "user" / "archives" / "archive" / "1"
    archive_dir.mkdir(parents=True)
    shutil.copyfile(sample, archive_dir / "sample.tib")
    size = int.from_bytes((vault / "unified_data.ds.0.szh").read_bytes(), "little")
    with (vault / "unified_data.ds.0").open("rb") as src:
        (target / store_name).write_bytes(src.read(size))
    if with_size_file:
        (target / (store_name + ".szh")).write_bytes(struct.pack("<Q", size))
    with peare_lib.open_file(str(archive_dir / "sample.tib")) as opener:
        assert _resource_hash(opener, "C:/PeareDedup/Input/variant.bin") == (
            "3a854bdbe1a78f1b6ed33072510c83ce8929f8b4417b6cecb9f564949d854366"
        )


def test_acronis15_external_dedup_disk(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = (Path(SAMPLES) / "acronis15-created" / "dedup-disk-20261116" /
              "Vault" / "disk-sample.tib")
    if not sample.is_file():
        pytest.skip("Acronis deduplicated disk sample not available")
    expected = {
        "alpha.bin": "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743",
        "same.bin": "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743",
        "variant.bin": "3a854bdbe1a78f1b6ed33072510c83ce8929f8b4417b6cecb9f564949d854366",
        "readme.txt": "2ae23fc848095781970d15b5abae83b57f1b5cff576bc891e3c1e423cbb1013c",
    }
    with peare_lib.open_file(str(sample)) as opener:
        for name, digest in expected.items():
            assert _resource_hash(opener, "Partition 1/" + name) == digest


@pytest.mark.parametrize("corrupt", [False, True])
def test_acronis15_external_dedup_disk_missing_or_corrupt(peare_lib, tmp_path, corrupt):
    from test_tibx_abi import _resource_hash

    root = Path(SAMPLES) / "acronis15-created" / "dedup-disk-20261116"
    sample = root / "Vault" / "disk-sample.tib"
    if not sample.is_file():
        pytest.skip("Acronis deduplicated disk sample not available")
    expected_boot = None
    if corrupt:
        with peare_lib.open_file(str(sample)) as original:
            resource = _HANDLE()
            assert original._L.peare_opener_open_resource_at(original._h, 0, 0, C.byref(resource)) == 0
            try:
                expected_boot = peare_lib.read_resource(resource, 0, 512)
            finally:
                original._L.peare_resource_destroy(resource)
    shutil.copyfile(sample, tmp_path / "disk-sample.tib")
    if corrupt:
        vault = root / "Vault"
        size = int.from_bytes((vault / "unified_data.ds.0.szh").read_bytes(), "little")
        with (vault / "unified_data.ds.0").open("rb") as src:
            data = bytearray(src.read(size))
        needle = (root / "Input" / "alpha.bin").read_bytes()[:128]
        archive = sample.read_bytes()
        end = len(data)
        pos = -1
        while end > 4:
            footer = data[end - 33:end]
            stored = struct.unpack_from("<Q", footer, 25)[0]
            start = end - 33 - stored
            assert start >= 4
            candidate = data.find(needle, start, end - 33)
            if footer[:16] in archive and candidate >= 0:
                pos = candidate
                break
            end = start
        assert pos >= 4, "The disk archive did not reference the expected data block"
        data[pos] ^= 1
        (tmp_path / "unified_data.ds.0").write_bytes(data)
        (tmp_path / "unified_data.ds.0.szh").write_bytes(struct.pack("<Q", size))
    with peare_lib.open_file(str(tmp_path / "disk-sample.tib")) as opener:
        if corrupt:
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
            try:
                assert peare_lib.read_resource(resource, 0, 512) == expected_boot
            finally:
                opener._L.peare_resource_destroy(resource)
        with pytest.raises((AssertionError, RuntimeError)):
            _resource_hash(opener, "Partition 1/alpha.bin")
        if corrupt:
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
            try:
                assert peare_lib.read_resource(resource, 0, 512) == expected_boot
            finally:
                opener._L.peare_resource_destroy(resource)


def test_acronis15_disk_backup(peare_lib):
    from test_tibx_abi import _resource_hash

    path = Path(SAMPLES) / "acronis15-created" / "disksample.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "a2cb82cb122a8afbe6be863f1daf80b2baa7cd1b5c45ee13fe24e153e6135bdb"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert _resource_hash(opener, "Partition 1", ranges=[(2 * 512, 251 * 512),
                                                             (253 * 512, 251 * 512)]) == [
            "e4ff0da6f56eaa47decc7e8fb23fef725a0bef9ef1a93ffb1cbaffbe9163b23a",
            "e4ff0da6f56eaa47decc7e8fb23fef725a0bef9ef1a93ffb1cbaffbe9163b23a",
        ]
        assert _resource_hash(opener, "Partition 1", ranges=[(253 * 512, 512), (253 * 512 - 15, 542)]) == [
            "b28139df2cb76769bfe70e73a24664437189d3244fbf0068a9d47ea6f9883688",
            "86099f4e9c3f9145826cc8b04f3caf8afaa70f04c310eb20224e96a15b21d733",
        ]
        assert _resource_hash(opener, "Partition 1/DATA/CHANGE.TXT") == (
            "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765"
        )
        assert _resource_hash(opener, "Partition 1/NOTEPAD.EXE") == (
            "c05fd6f341b2bcb5e482deff5baf53903910a42d55e4b2f23a7053cae5ddbdae"
        )


@pytest.mark.parametrize(("sample", "partition", "archive_hash"), [
    ("disksample.tib", "Partition 1", "a2cb82cb122a8afbe6be863f1daf80b2baa7cd1b5c45ee13fe24e153e6135bdb"),
    ("diskmodern.tibx", "disk1/part1", "8a231ed9edb433742888d77baed447c536ad4d8815b65f2bcbdf0a3a0d534b11"),
])
def test_acronis15_disk_backup_complete_partition_matches_source(peare_lib, sample, partition, archive_hash):
    from test_tibx_abi import _resource_hash

    path = Path(SAMPLES) / "acronis15-created" / sample
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == archive_hash
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert _resource_hash(opener, partition) == (
            "7ea8f3db32ad7f49bfba7a60927c3980591e12cca9f891b5e28a807d3b72cd8b"
        )
        assert _resource_hash(opener, partition, original=True) == (
            "b326bc01e57846962e98e4e1e3e735edcc8be51992811689e768bcb33c2f310d"
        )


def test_acronis15_disk_backup_filtered_fat_directory(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "disksample.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "a2cb82cb122a8afbe6be863f1daf80b2baa7cd1b5c45ee13fe24e153e6135bdb"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        rows = opener.normalized_tree(max_depth=4)
        names = {row["path"].rsplit("/", 1)[-1].upper() for row in rows
                 if row["path"].startswith("Partition 1/") and row["path"].count("/") == 1}
        assert names == {"DATA", "NOTEPAD.EXE"}
        nested = {row["path"].rsplit("/", 1)[-1].upper() for row in rows
                  if row["path"].startswith("Partition 1/DATA/")}
        assert nested == {"CHANGE.TXT"}


def test_acronis15_indexed_descriptor_read_failure(peare_lib):
    from test_callback_source_abi import CallbackSource
    from test_tibx_abi import _open_source_with_password

    path = Path(SAMPLES) / "acronis15-created" / "disksample.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "a2cb82cb122a8afbe6be863f1daf80b2baa7cd1b5c45ee13fe24e153e6135bdb"
    )

    class FailingDescriptor(CallbackSource):
        failures = 0

        def read(self, context, offset, buffer, capacity, out_read):
            if offset == 143555 and capacity == 2:
                self.failures += 1
                return 13
            return super().read(context, offset, buffer, capacity, out_read)

    source = FailingDescriptor(peare_lib, path)
    opener = None
    try:
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        assert source.failures > 0
        assert status != 0 and opener is None
    finally:
        if opener is not None:
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


def test_acronis15_disk_backup_original_read_bounds(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "disksample.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            context = _Context()
            assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
            size = int(context.data_size)
            opener._L.peare_resource_context_free(C.byref(context))
            original = peare_lib.read_resource_original(resource, 253 * 512, 512)
            restored = peare_lib.read_resource(resource, 253 * 512, 512)
            assert original == bytes(512)
            assert hashlib.sha256(restored).hexdigest() == (
                "b28139df2cb76769bfe70e73a24664437189d3244fbf0068a9d47ea6f9883688"
            )
            assert peare_lib.read_resource_original(resource, size, 1) == b""
            count = C.c_size_t(1)
            buffer = (C.c_uint8 * 1)()
            assert opener._L.peare_resource_read_original(
                resource, size + 1, buffer, 1, C.byref(count)
            ) == 5
            assert count.value == 0
        finally:
            opener._L.peare_resource_destroy(resource)


def test_acronis15_original_and_restored_concurrent_reads(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "disksample.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "a2cb82cb122a8afbe6be863f1daf80b2baa7cd1b5c45ee13fe24e153e6135bdb"
    )
    with peare_lib.open_file(str(path)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            barrier = threading.Barrier(8)

            def worker(index):
                barrier.wait(timeout=30)
                read = peare_lib.read_resource_original if index % 2 else peare_lib.read_resource
                digest = hashlib.sha256(read(resource, 253 * 512, 512)).hexdigest()
                return digest

            with ThreadPoolExecutor(max_workers=8) as executor:
                result = list(executor.map(worker, range(8)))
            raw = hashlib.sha256(bytes(512)).hexdigest()
            restored = "b28139df2cb76769bfe70e73a24664437189d3244fbf0068a9d47ea6f9883688"
            assert result == [restored if index % 2 == 0 else raw for index in range(8)]
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize("failure_offset", [139516, 139543])
def test_acronis15_disk_backup_filter_read_error(peare_lib, failure_offset):
    from test_callback_source_abi import CallbackSource
    from test_tibx_abi import _directory_source, _open_source_with_password

    path = Path(SAMPLES) / "acronis15-created" / "disksample.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "a2cb82cb122a8afbe6be863f1daf80b2baa7cd1b5c45ee13fe24e153e6135bdb"
    )

    class FailingFilter(CallbackSource):
        def __init__(self, lib, sample):
            self.fail_filter = False
            self.filter_failures = 0
            super().__init__(lib, sample)

        def read(self, context, offset, buffer, capacity, out_read):
            if self.fail_filter and offset <= failure_offset < offset + capacity:
                self.filter_failures += 1
                return 13
            return super().read(context, offset, buffer, capacity, out_read)

    source = FailingFilter(peare_lib, path)
    root = child = None
    partition_source = _HANDLE()
    try:
        status, root = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and root is not None
        partition_source = _directory_source(root, "Partition 1")
        source.fail_filter = True
        status, child = _open_source_with_password(peare_lib, partition_source, None)
        assert source.filter_failures > 0
        assert status != 0 and child is None
    finally:
        if child is not None:
            child.close()
        if partition_source:
            peare_lib._lib.peare_source_destroy(partition_source)
        if root is not None:
            root.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@pytest.mark.parametrize("fail_from_start", [False, True])
def test_acronis15_restored_read_filter_error_keeps_original(peare_lib, fail_from_start):
    from test_callback_source_abi import CallbackSource
    from test_tibx_abi import _open_source_with_password

    path = Path(SAMPLES) / "acronis15-created" / "disksample.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 disk backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "a2cb82cb122a8afbe6be863f1daf80b2baa7cd1b5c45ee13fe24e153e6135bdb"
    )

    class FailingFilter(CallbackSource):
        fail_filter = False
        failures = 0

        def read(self, context, offset, buffer, capacity, out_read):
            if self.fail_filter and offset <= 139516 < offset + capacity:
                self.failures += 1
                return 13
            return super().read(context, offset, buffer, capacity, out_read)

    source = FailingFilter(peare_lib, path)
    opener = None
    resource = _HANDLE()
    try:
        source.fail_filter = fail_from_start
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and opener is not None
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        source.fail_filter = True
        assert peare_lib.read_resource_original(resource, 0, 512)[510:512] == b"\x55\xaa"
        with pytest.raises(RuntimeError):
            peare_lib.read_resource(resource, 0, 512)
        assert source.failures > 0
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if opener is not None:
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


def test_acronis15_file_backup(peare_lib):
    path = Path(SAMPLES) / "acronis15-created" / "legacy.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 file backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "ca90eb546c6dfea7da6e3c0d5ff922fee85760bd686ec280667cab48c8d3278b"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert opener.container_format() == "TIB"
        rows = opener.normalized_tree(max_depth=4)
        assert len({row["path"] for row in rows}) == len(rows)
        item = next(row for row in rows if row["path"] == "C:/PeareInput/sample.txt")
        assert not item["is_container"]
        assert item["size"] == 23
        assert item["sha256"] == (
            "aac8714a07f6e40e95d7fa5ac6f2acd42417eb86b8f1cbd76076fc7726d7c62c"
        )


def test_acronis15_smb_hybrid_file_backup(peare_lib):
    from test_tibx_abi import _resource_hash

    directory = Path(SAMPLES) / "point11-acp15-smb-hybrid-20261123"
    sample = directory / "point11_equal_size_2026_11_23_18_03_08_934F.TIB"
    if not sample.is_file():
        pytest.skip("Acronis Cyber Protect 15 SMB hybrid sample not available")
    assert hashlib.sha256(sample.read_bytes()).hexdigest() == (
        "99c56252c1c614e1aa10590c0c9cca080cd82cc8694a1ec22285aebfdbb94227"
    )
    expected = {
        "snsrb01539/PearePoint11Source2/same.bin": (
            "e6e7546e7ce4e977f2002403d9771f7df1fba091d660ac1e27d2e2361c42c743"
        ),
        "snsrb01539/PearePoint11Source2/variant.bin": (
            "3a854bdbe1a78f1b6ed33072510c83ce8929f8b4417b6cecb9f564949d854366"
        ),
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=5)
        actual = {row["path"]: row for row in rows if row["path"] in expected}
        assert set(actual) == set(expected), rows
        assert all(actual[path]["size"] == 2_097_152 for path in expected)
        for path, digest in expected.items():
            assert _resource_hash(opener, path) == digest


def test_acronis15_smb_hybrid_incremental_backup(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = (Path(SAMPLES) / "point11-acp15-smb-hybrid-20261123" /
              "point12_incremental_2026_11_23_18_03_08_934F2.TIB")
    if not sample.is_file():
        pytest.skip("Acronis Cyber Protect 15 SMB incremental sample not available")
    assert hashlib.sha256(sample.read_bytes()).hexdigest() == (
        "4b2e1835c6189677ed919eff544c9ba2cdc9d7ecb325a06d338d80f4ab63fd7b"
    )
    path = "snsrb01539/PearePoint11Source2/incremental-hosts.txt"
    with peare_lib.open_file(str(sample)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree(max_depth=5)}
        assert path in rows, rows
        assert rows[path]["size"] == 824
        assert _resource_hash(opener, path) == (
            "2d6bdfb341be3a6234b24742377f93aa7c7cfb0d9fd64efa9282c87852e57085"
        )


@pytest.mark.parametrize(("selected", "expected"), [
    (0, "aac8714a07f6e40e95d7fa5ac6f2acd42417eb86b8f1cbd76076fc7726d7c62c"),
    (1, "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff"),
])
def test_acronis15_incremental_file_backup(peare_lib, selected, expected):
    from test_tibx_abi import _resource_hash

    directory = Path(SAMPLES) / "acronis15-created" / "legacy-incremental"
    parts = [
        ("legacy_2026_11_14_20_26_57_651F.TIB",
         "ca90eb546c6dfea7da6e3c0d5ff922fee85760bd686ec280667cab48c8d3278b"),
        ("legacy_2026_11_14_20_26_57_651F2.TIB",
         "76d053223a502613f9fa69cfe39233b34ed519d6674cd290acfc7ea74aba54a3"),
    ]
    if not all((directory / name).is_file() for name, _ in parts):
        pytest.skip("Acronis Cyber Protect 15 incremental backup not available")
    for name, digest in parts:
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest
    with peare_lib.open_file(str(directory / parts[selected][0])) as opener:
        assert opener is not None
        assert _resource_hash(opener, "C:/PeareInput/sample.txt") == expected


@pytest.mark.parametrize(("selected", "expected"), [
    (0, {"change.txt": "fe5aafb43acf2870bfe6900d723035d14da8b1f05c098efe18702050cd2ad8f3",
         "keep.txt": "5012e9fa783358f184e3231476c6a5700685f329105970009cd80a1e3bdf4408",
         "remove.txt": "513e2bfa6e9d3ff64397f9fe2ca9e6ebfe64b13d4b2f1942070bb966eecbb60e"}),
    (1, {"added.txt": "fe1761ced5e5f40fc55cb50886c72d3059c0e62827bfcdf6e6412e237d052b11",
         "change.txt": "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765",
         "keep.txt": "5012e9fa783358f184e3231476c6a5700685f329105970009cd80a1e3bdf4408"}),
    (2, {"added.txt": "fe1761ced5e5f40fc55cb50886c72d3059c0e62827bfcdf6e6412e237d052b11",
         "change.txt": "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765"}),
    (3, {"added.txt": "fe1761ced5e5f40fc55cb50886c72d3059c0e62827bfcdf6e6412e237d052b11",
         "change.txt": "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765"}),
    (4, {"added.txt": "fe1761ced5e5f40fc55cb50886c72d3059c0e62827bfcdf6e6412e237d052b11",
         "change.txt": "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765",
         "remove.txt": "68d949ba7f391a97d5a39228a1334320b89f8df7ff3b3b3b6d198cbf98852614",
         "inside.txt": "b61e1830d249054aec60c9d8d1e4eeb285d5cc8104ce79acdc66ab7cb9579c47"}),
    (5, {"added.txt": "fe1761ced5e5f40fc55cb50886c72d3059c0e62827bfcdf6e6412e237d052b11",
         "change.txt": "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765",
         "remove.txt": "68d949ba7f391a97d5a39228a1334320b89f8df7ff3b3b3b6d198cbf98852614"}),
])
def test_acronis15_incremental_file_deletion(peare_lib, selected, expected):
    directory = Path(SAMPLES) / "acronis15-created" / "chain-multi"
    parts = [
        ("chainmulti_2026_11_14_22_56_17_341F.TIB", "936a0860aea8ba86721f3d66143c040ad515115f670d3dcebca1b584742a31f5"),
        ("chainmulti_2026_11_14_22_56_17_341F2.TIB", "142ae3ac5c56bc114edd4431e56ac0a9fb85419c7b45dd34e4a7163e14943bcc"),
        ("chainmulti_2026_11_14_22_56_17_341F3.TIB", "088efead35d684feab56396a10cce916cc403c919568395b1d60efc5be3f14d1"),
        ("chainmulti_2026_11_14_22_56_17_341F4.TIB", "12cb5dee7bedcb0872ef5aa1515c3ee34dbbe29a020c33e0a6bd78744e9597ea"),
        ("chainmulti_2026_11_14_22_56_17_341F5.TIB", "4d30370db24eacb95d25c6bdca01253d69c3f1e2c5d21f8dd4c0c97f049b625d"),
        ("chainmulti_2026_11_14_22_56_17_341F6.TIB", "84d99b0fec3cbe7434b14b6975f2b38ce207f44a35b8857110c290ba64d54df0"),
    ]
    if not all((directory / name).is_file() for name, _ in parts):
        pytest.skip("Acronis Cyber Protect 15 incremental backup not available")
    for name, digest in parts:
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest
    with peare_lib.open_file(str(directory / parts[selected][0])) as opener:
        assert opener is not None
        rows = opener.normalized_tree(max_depth=5)
        actual = {row["path"].rsplit("/", 1)[-1]: row["sha256"]
                  for row in rows if row["path"].startswith("C:/PeareChain/")
                  and not row["is_container"]}
        assert actual == expected
        assert any(row["path"] == "C:/PeareChain/nested" and row["is_container"]
                   for row in rows) == (selected == 4)


@pytest.mark.parametrize(("selected", "removed_present"), [(0, True), (1, False)])
def test_acronis15_encrypted_incremental_deletion(peare_lib, selected, removed_present):
    directory = Path(SAMPLES) / "acronis15-created" / "chain-secure"
    parts = [
        ("chainsecure_2026_11_14_23_32_04_974F.TIB", "f8256e4948f66e853f18c4c0e6a6f5ec4da1ae123c03feda8e31388400d1aada"),
        ("chainsecure_2026_11_14_23_32_04_974F2.TIB", "5f581feaf55dcd0bb13523305682eff56f77e1b125f5219818b03d5937634496"),
    ]
    if not all((directory / name).is_file() for name, _ in parts):
        pytest.skip("Acronis Cyber Protect 15 encrypted chain not available")
    for name, digest in parts:
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest
    expected = {"added.txt": "fe1761ced5e5f40fc55cb50886c72d3059c0e62827bfcdf6e6412e237d052b11",
                "change.txt": "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765"}
    if removed_present:
        expected["remove.txt"] = "68d949ba7f391a97d5a39228a1334320b89f8df7ff3b3b3b6d198cbf98852614"
    with peare_lib.open_file(str(directory / parts[selected][0]), "PeareTest2026") as opener:
        assert opener is not None
        rows = opener.normalized_tree(max_depth=5)
        actual = {row["path"].rsplit("/", 1)[-1]: row["sha256"]
                  for row in rows if row["path"].startswith("C:/PeareChain/")
                  and not row["is_container"]}
        assert actual == expected


def test_acronis15_incremental_file_backup_missing_base(peare_lib, tmp_path):
    directory = Path(SAMPLES) / "acronis15-created" / "legacy-incremental"
    part = directory / "legacy_2026_11_14_20_26_57_651F2.TIB"
    if not part.is_file():
        pytest.skip("Acronis Cyber Protect 15 incremental backup not available")
    isolated = tmp_path / part.name
    shutil.copyfile(part, isolated)
    assert peare_lib.open_file(str(isolated)) is None


@pytest.mark.parametrize(("selected", "expected"), [
    (0, "aac8714a07f6e40e95d7fa5ac6f2acd42417eb86b8f1cbd76076fc7726d7c62c"),
    (1, "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff"),
])
def test_acronis15_encrypted_incremental_file_backup(peare_lib, selected, expected):
    from test_tibx_abi import _resource_hash

    directory = Path(SAMPLES) / "acronis15-created" / "legacy-aes-incremental"
    parts = [
        ("legacy-aes_2026_11_14_20_40_04_808F.TIB",
         "65735ab873997c293d2386278952023931aac3b2bcb8d0c2a14bd00ae34a4804"),
        ("legacy-aes_2026_11_14_20_40_04_808F2.TIB",
         "93a73ef8d2e13b02edc308c0443977e322fc37adc8ba5e40a1639b96bb7f3f9c"),
    ]
    if not all((directory / name).is_file() for name, _ in parts):
        pytest.skip("Acronis Cyber Protect 15 encrypted incremental backup not available")
    for name, digest in parts:
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest
    with peare_lib.open_file(str(directory / parts[selected][0]), "PeareTest2026") as opener:
        assert opener is not None
        assert _resource_hash(opener, "C:/PeareInput/sample.txt", "PeareTest2026") == expected


def test_acronis15_max_compression_file_backup(peare_lib):
    from test_tibx_abi import _resource_hash

    path = Path(SAMPLES) / "acronis15-created" / "legacy-max.tib"
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 compressed file backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == (
        "11fb45a71ac31136c5df975f56b7e5130f3c8b68c34ee751d496e4d448427f73"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert opener.container_format() == "TIB"
        assert _resource_hash(opener, "C:/Windows/System32/notepad.exe") == (
            "c05fd6f341b2bcb5e482deff5baf53903910a42d55e4b2f23a7053cae5ddbdae"
        )


ACRONIS15_SPLIT = [
    ("legacysplit_2026_11_14_21_12_08_993F.TIB",
     "8f195dbf8d9149ed5ec76808920849b0982a9f9e2cd50c15d117a00a2d6aedc2"),
    ("legacysplit_2026_11_14_21_12_08_993F2.TIB",
     "ffefae2130515e3e4312f229d9a04ed6dbc44d7829731bdcbe23bfdd2db8395b"),
    ("legacysplit_2026_11_14_21_12_08_993F3.TIB",
     "d9e57a0233aec756ff8cdea5dc112edf22926d320116d4a0c489e754990af4a4"),
]

ACRONIS15_SPLIT_AES = [
    ("splitsecure_2026_11_14_21_31_54_947F.TIB",
     "47dfb4065f120ea157fb4b08efba785e6e4e9db54207e69545f6df79b1d03036"),
    ("splitsecure_2026_11_14_21_31_54_947F2.TIB",
     "22a8f8700bd6bfe06f57fb67ad065b77d75d6da37e6aad248bc194b6ea5aa9e1"),
    ("splitsecure_2026_11_14_21_31_54_947F3.TIB",
     "e18eadecea6c5ba40298210549166536253b00f14bd70a58e7323188c97056c0"),
]


@pytest.mark.parametrize("selected", range(3))
def test_acronis15_split_file_backup(peare_lib, selected):
    from test_tibx_abi import _resource_hash

    directory = Path(SAMPLES) / "acronis15-created"
    paths = [directory / name for name, _ in ACRONIS15_SPLIT]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 split file backup not available")
    for path, (_, digest) in zip(paths, ACRONIS15_SPLIT):
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    with peare_lib.open_file(str(paths[selected])) as opener:
        assert opener is not None
        assert opener.container_format() == "TIB"
        assert _resource_hash(opener, "C:/Windows/System32/ntdll.dll") == (
            "43a5a732667cad04776a1ccf51e152c48705ad3ba486e0c628b7386e9a7138d7"
        )


def test_acronis15_split_file_backup_missing_middle(peare_lib, tmp_path):
    directory = Path(SAMPLES) / "acronis15-created"
    paths = [directory / name for name, _ in ACRONIS15_SPLIT]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 split file backup not available")
    for path in (paths[0], paths[2]):
        shutil.copyfile(path, tmp_path / path.name)
    assert peare_lib.open_file(str(tmp_path / paths[0].name)) is None


@pytest.mark.parametrize("selected", range(3))
@pytest.mark.parametrize("names", [
    ("archive1.tib", "archive12.tib", "archive13.tib"),
    ("archive1.tib", "archive2.tib", "archive3.tib"),
])
def test_acronis15_split_file_backup_numeric_basename(peare_lib, tmp_path, selected, names):
    from test_tibx_abi import _resource_hash

    directory = Path(SAMPLES) / "acronis15-created"
    paths = [directory / name for name, _ in ACRONIS15_SPLIT]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 split file backup not available")
    for source, name in zip(paths, names):
        shutil.copyfile(source, tmp_path / name)
    with peare_lib.open_file(str(tmp_path / names[selected])) as opener:
        assert opener is not None
        assert _resource_hash(opener, "C:/Windows/System32/ntdll.dll") == (
            "43a5a732667cad04776a1ccf51e152c48705ad3ba486e0c628b7386e9a7138d7"
        )


def test_acronis15_split_file_backup_ambiguous_basename(peare_lib, tmp_path):
    directory = Path(SAMPLES) / "acronis15-created"
    paths = [directory / name for name, _ in ACRONIS15_SPLIT]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 split file backup not available")
    for source, names in zip(paths, [("archive1.tib",), ("archive2.tib", "archive12.tib"),
                                    ("archive3.tib", "archive13.tib")]):
        for name in names:
            shutil.copyfile(source, tmp_path / name)
    assert peare_lib.open_file(str(tmp_path / "archive1.tib")) is None


@pytest.mark.parametrize("selected", range(3))
def test_acronis15_split_encrypted_file_backup(peare_lib, selected):
    from test_tibx_abi import _resource_hash

    directory = Path(SAMPLES) / "acronis15-created"
    paths = [directory / name for name, _ in ACRONIS15_SPLIT_AES]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 encrypted split backup not available")
    for path, (_, digest) in zip(paths, ACRONIS15_SPLIT_AES):
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    assert peare_lib.open_file(str(paths[selected]), "incorrect") is None
    assert peare_lib.last_open_status == 15
    with peare_lib.open_file(str(paths[selected]), "PeareTest2026") as opener:
        assert opener is not None
        assert opener.container_format() == "TIB"
        assert _resource_hash(opener, "C:/Windows/System32/ntdll.dll", "PeareTest2026") == (
            "43a5a732667cad04776a1ccf51e152c48705ad3ba486e0c628b7386e9a7138d7"
        )


@pytest.mark.parametrize(("name", "archive_hash"), [
    ("legacy-aes.tib", "65735ab873997c293d2386278952023931aac3b2bcb8d0c2a14bd00ae34a4804"),
    ("legacy-aes128.tib", "0a62bdb4c3fdbec6126b0cfa5c3746d4c12a2d24da606cc06c8d72c3cc44080a"),
    ("legacy-aes192.tib", "a46e69bdce39978345aa91a08b87c5f941c9b72ecb5fc0f5de30a7d5f99a9ed6"),
])
def test_acronis15_encrypted_file_backup(peare_lib, name, archive_hash):
    path = Path(SAMPLES) / "acronis15-created" / name
    if not path.is_file():
        pytest.skip("Acronis Cyber Protect 15 encrypted file backup not available")
    assert hashlib.sha256(path.read_bytes()).hexdigest() == archive_hash
    assert peare_lib.open_file(str(path)) is None
    assert peare_lib.last_open_status == 14
    assert peare_lib.open_file(str(path), "incorrect") is None
    assert peare_lib.last_open_status == 15
    with peare_lib.open_file(str(path), "PeareTest2026") as opener:
        assert opener is not None
        assert opener.container_format() == "TIB"
        rows = opener.normalized_tree(max_depth=4)
        assert len({row["path"] for row in rows}) == len(rows)
        item = next(row for row in rows if row["path"] == "C:/PeareInput/sample.txt")
        assert not item["is_container"]
        assert item["size"] == 23
        assert item["sha256"] == (
            "aac8714a07f6e40e95d7fa5ac6f2acd42417eb86b8f1cbd76076fc7726d7c62c"
        )


def test_unrelated_tib_extension_is_not_acronis(peare_lib):
    path = Path(SAMPLES) / "KRON200_0.TIB"
    assert path.is_file()
    opener = peare_lib.open_file(str(path))
    if opener:
        try:
            assert opener.container_format() not in ("TIB", "TIBX")
        finally:
            opener.close()


def test_tib_split_missing_parts(peare_lib, tmp_path):
    source = Path(SAMPLES) / "TeamViewer_full_b1_s1_v5.tib"
    if not source.is_file():
        pytest.skip("Independent final TIB volume not available")
    assert hashlib.sha1(source.read_bytes()).hexdigest() == "92683da2c4c0deb77980fb70bfd2af92a690142a"
    isolated = tmp_path / source.name
    shutil.copyfile(source, isolated)
    opener = peare_lib.open_file(str(isolated))
    if opener:
        opener.close()
    assert opener is None


@pytest.mark.parametrize(("sample", "map_offset"), [
    ("c_intel_full_b1_s1_v1.tib", 0xA55),
    ("c_intel_fullcompression_full_b1_s1_v1.tib", 0xA55),
    ("c_intel_fullcompression_full_b2_s1_v1.tib", 0x6E7),
])
def test_tib_intel_short_block_recorded_digest(sample, map_offset):
    archive = (Path(SAMPLES) / sample).read_bytes()
    assert archive[0x20:0x21] == b"m"
    assert archive[map_offset - 1:map_offset] == b"l"
    payload = zlib.decompress(archive[0x21:])
    metadata = zlib.decompress(archive[map_offset:])
    assert len(payload) == 27699
    assert len(metadata) == 72
    assert metadata[36:52] == hashlib.md5(payload).digest()
    assert metadata[36:52] != hashlib.md5(payload + b"\0").digest()


@pytest.mark.parametrize(("sample", "map_offset"), [
    ("c_intel_full_b1_s1_v1.tib", 0xA55),
    ("c_intel_fullcompression_full_b1_s1_v1.tib", 0xA55),
    ("c_intel_fullcompression_full_b2_s1_v1.tib", 0x6E7),
])
def test_tib_hash_block_map_direct_record_reference(peare_lib, sample, map_offset):
    from test_tibx_abi import _resource_hash

    path = Path(SAMPLES) / sample
    archive = path.read_bytes()
    assert archive[map_offset - 1:map_offset] == b"l"
    block_map = zlib.decompress(archive[map_offset:])
    version, options, hash_id, hash_size, group_count = struct.unpack_from("<BBBBH", block_map)
    assert (version, options, hash_id, hash_size, group_count) == (1, 2, 0, 16, 1)
    name_size = struct.unpack_from("<H", block_map, 6)[0]
    assert name_size == 0
    cursor = 8 + name_size
    length, count = struct.unpack_from("<QI", block_map, cursor)
    cursor += 12
    logical, reference = struct.unpack_from("<QQ", block_map, cursor)
    cursor += 16
    assert (length, count, logical, reference) == (27700, 1, 0, 0)
    assert len(block_map[cursor:cursor + hash_size]) == hash_size
    header_size = struct.unpack_from("<H", archive, 4)[0]
    stored_base = struct.unpack_from("<I", archive, 28)[0] if header_size >= 32 else 0
    base = stored_base or header_size
    assert archive[base + reference:base + reference + 1] == b"m"
    payload = zlib.decompress(archive[base + reference + 1:])
    assert len(payload) == 27699
    assert hashlib.sha256(payload).hexdigest() == "e5868beca0dfe44c75547ed847d4fbea8c74d44aeea8f80e000f58f09b46502e"
    # An Acronis restore of the first sample matches the zero-padded result.
    assert hashlib.sha256(payload + b"\0").hexdigest() == (
        "a4bd62c527d911eaf4a8cea56643b432a842555d16dc60ab7df33a0478fb6139"
    )
    with peare_lib.open_file(str(path)) as opener:
        assert opener is not None
        assert _resource_hash(opener, "C:/Intel/Logs/IntelGFXCoin.log") == hashlib.sha256(payload + b"\0").hexdigest()


@pytest.mark.parametrize("sample", [
    "WinRAR_full_b1_s1_v1.tib",
    "ADD_full_b1_s1_v1.tib",
    "2020.09.27 \u041e\u0431\u043b\u0430\u043a\u0430 \u0414\u0430\u043b\u0438_full_b1_s1_v1.tib",
    "001_full_b1_s1_v1.tib",
    "2015q4-2018q4.tib",
])
def test_tib_encrypted_password_status(peare_lib, sample):
    path = Path(SAMPLES) / sample
    if not path.is_file():
        pytest.skip("Independent encrypted TIB sample not available")
    assert peare_lib.open_file(str(path)) is None
    assert peare_lib.last_open_status == 14
    assert peare_lib.open_file(str(path), "Peare-incorrect-password") is None
    assert peare_lib.last_open_status == 15


def test_tib_file_backup_paths_and_content(peare_lib):
    from test_tibx_abi import _resource_hash

    path = Path(SAMPLES) / "testAcronis_full_b1_s1_v1.tib"
    assert path.is_file()
    with peare_lib.open_file(str(path)) as opener:
        assert opener.container_format() == "TIB"
        rows = opener.normalized_tree(max_depth=3)
        paths = [row["path"] for row in rows]
        assert len(paths) == len(set(paths))
        assert "C:/testAcronis/100MB.bin" in paths
        assert not next(row for row in rows if row["path"] == "C:/testAcronis/100MB.bin")["is_container"]
        assert _resource_hash(opener, "C:/testAcronis/100MB.bin") == (
            "20492a4d0d84f8beb1767f6616229f85d44c2827b64bdbfb260ee12fa1109e0e"
        )
        assert _resource_hash(opener, "C:/testAcronis/100MB.bin:Zone.Identifier:$DATA") == (
            "f870e553a2c6289e1ee41c5d06eb49c0ba2efcdeeee0dccfc37dc589da561bd3"
        )


@pytest.mark.parametrize("sample", [
    "c_intel_full_b1_s1_v1.tib",
    "c_intel_fullcompression_full_b1_s1_v1.tib",
    "c_intel_fullcompression_full_b2_s1_v1.tib",
])
def test_tib_intel_logs_empty_file_and_short_block(peare_lib, sample):
    from peare_opener import Opener
    from test_tibx_abi import _directory_source, _open_source_with_password

    path = Path(SAMPLES) / sample
    assert path.is_file()
    with ExitStack() as stack:
        root = peare_lib.open_file(str(path))
        assert root is not None
        current = stack.enter_context(root)
        for name in ("C:", "Intel", "Logs"):
            source = _directory_source(current, name)
            try:
                status, child = _open_source_with_password(peare_lib, source, None)
            finally:
                peare_lib._lib.peare_source_destroy(source)
            assert status == 0 and child is not None, f"opening {name}: {status}"
            assert isinstance(child, Opener)
            current = stack.enter_context(child)
        items = {}
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
                    assert name not in items
                    if name == "IntelCPHS.log":
                        assert size == 0
                        assert peare_lib.read_resource(resource, 0, 1) == b""
                    elif name == "IntelGFXCoin.log":
                        assert size == 27700
                        data = peare_lib.read_resource(resource, 0, size)
                        assert len(data) == size
                        assert hashlib.sha256(data[:27699]).hexdigest() == (
                            "e5868beca0dfe44c75547ed847d4fbea8c74d44aeea8f80e000f58f09b46502e"
                        )
                        # This checks Peare's short-block padding, not the original final byte.
                        assert data[27699:] == b"\0"
                        reference = data[:27699] + b"\0"
                        for offset, length in ((0, 1), (1, 2), (27697, 8),
                                               (27698, 8), (27699, 8), (27700, 1)):
                            assert peare_lib.read_resource(resource, offset, length) == reference[offset:offset + length]
                        with pytest.raises(RuntimeError):
                            peare_lib.read_resource(resource, size + 1, 1)

                        barrier = threading.Barrier(4)

                        def read_positioned(seed):
                            rng = random.Random(seed)
                            barrier.wait(timeout=30)
                            for _ in range(100):
                                offset = rng.randrange(size)
                                length = rng.randrange(1, 200)
                                actual = peare_lib.read_resource(resource, offset, length)
                                assert actual == reference[offset:offset + length], (offset, length)

                        with ThreadPoolExecutor(max_workers=4) as pool:
                            futures = [pool.submit(read_positioned, seed) for seed in range(4)]
                            for future in futures:
                                future.result()
                    items[name] = size
                finally:
                    current._L.peare_resource_destroy(resource)
        assert set(items) == {"IntelCPHS.log", "IntelGFXCoin.log"}


def test_tib_file_read_failure_preserves_buffer(peare_lib):
    from test_callback_source_abi import CallbackSource, _find_resource, _open_parts
    from test_tibx_abi import _directory_source, _open_source_with_password

    paths = [Path(SAMPLES) / "acronis15-created" / name for name, _ in ACRONIS15_SPLIT]
    if not all(path.is_file() for path in paths):
        pytest.skip("The independent split TIB sample is not available")

    class FailingSource(CallbackSource):
        armed = False
        failures = 0

        def read(self, context, offset, buffer, capacity, out_read):
            if self.armed:
                self.failures += 1
                return 13
            return super().read(context, offset, buffer, capacity, out_read)

    sources = [FailingSource(peare_lib, path) for path in paths]
    resource = _HANDLE()
    try:
        with ExitStack() as stack:
            status, root = _open_parts(peare_lib, sources)
            assert status == 0 and root is not None
            current = stack.enter_context(root)
            for name in ("C:", "Windows", "System32"):
                child_source = _directory_source(current, name)
                try:
                    status, child = _open_source_with_password(peare_lib, child_source, None)
                finally:
                    peare_lib._lib.peare_source_destroy(child_source)
                assert status == 0 and child is not None
                current = stack.enter_context(child)
            resource, size = _find_resource(current, "ntdll.dll")
            assert peare_lib.read_resource(resource, size - 1, 1)
            for source in sources:
                source.armed = True
            data = (C.c_uint8 * 64)(*([0xa5] * 64))
            count = C.c_size_t(1)
            status = current._L.peare_resource_read(resource, 0, data, len(data), C.byref(count))
            assert sum(source.failures for source in sources) > 0
            assert status != 0 and count.value == 0
            assert bytes(data) == bytes([0xa5]) * len(data)
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


def test_tib_file_backup_positioned_reads(peare_lib):
    path = Path(SAMPLES) / "testAcronis_full_b1_s1_v1.tib"

    def find_resource(opener, name):
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = _HANDLE()
                assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(resource)) == 0
                context = _Context()
                assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
                actual = context.identifier_utf8.to_bytes().decode("utf-8")
                opener._L.peare_resource_context_free(C.byref(context))
                if actual == name:
                    return resource
                opener._L.peare_resource_destroy(resource)
        pytest.fail("TIB resource not found: " + name)

    with peare_lib.open_file(str(path)) as root, ExitStack() as stack:
        current = root
        for name in ("C:", "testAcronis"):
            resource = find_resource(current, name)
            source = _HANDLE()
            try:
                assert current._L.peare_resource_get_source(resource, C.byref(source)) == 0
                child = peare_lib._open_source(source)
                assert child is not None
                current = stack.enter_context(child)
            finally:
                if source:
                    current._L.peare_source_destroy(source)
                current._L.peare_resource_destroy(resource)

        resource = find_resource(current, "100MB.bin:Zone.Identifier:$DATA")
        try:
            payload = peare_lib.read_resource(resource, 0, 109)
            assert hashlib.sha256(payload).hexdigest() == (
                "f870e553a2c6289e1ee41c5d06eb49c0ba2efcdeeee0dccfc37dc589da561bd3"
            )
            # This sample is unencrypted; these are ABI regressions, not CBC proof.
            for offset, length in ((108, 8), (17, 23), (0, 1), (45, 50), (109, 8),
                                   (15, 2), (16, 16), (31, 2), (32, 16), (95, 14)):
                assert peare_lib.read_resource(resource, offset, length) == payload[offset:offset + length]
            requests = [(offset, 13) for offset in range(len(payload))]
            with ThreadPoolExecutor(max_workers=4) as executor:
                chunks = list(executor.map(lambda request: peare_lib.read_resource(resource, *request), requests))
            assert chunks == [payload[offset:offset + length] for offset, length in requests]
            with pytest.raises(RuntimeError):
                peare_lib.read_resource(resource, 110, 1)
        finally:
            current._L.peare_resource_destroy(resource)


def test_tib_sector_ntfs(peare_lib):
    path = Path(SAMPLES) / "win2k-sp4.tib"
    if not path.is_file():
        pytest.skip("The independent Windows 2000 TIB sample is not available")
    with peare_lib.open_file(str(path)) as opener:
        assert opener.container_format() == "TIB"
        roots = opener.normalized_tree(max_depth=0, recurse_containers=False)
        assert [(row["path"], row["format"], row["is_container"]) for row in roots] == [
            ("Partition 1", "NTFS", True)]
        assert roots[0]["size"] == 20948697 * 512
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        source = _HANDLE()
        try:
            windows = [
                (0, 12000, "6b598e1b5993b552619778594a31ec110b15638bc119d833713b7858aaa64aa8"),
                (10, 12000, "4e72e64d63632273a5683dbf15ad26b9a65f24ccf7efb4009aace117702aeef4"),
                (100, 12000, "ff6698a6e831ffcf47af2fed388ffc262f319e72b26cd140929d1e19b1246ad4"),
                (37346, 10000, "95b532cc4381affdff0d956e12520a04129ed49d37e154228368fe5621f0b9a2"),
            ]
            for index, size, expected_hash in windows:
                data = peare_lib.read_resource(resource, index * 256 * 1024 + 250000, size)
                assert len(data) == size
                assert hashlib.sha256(data).hexdigest() == expected_hash
            boundary = 37347 * 256 * 1024
            before = peare_lib.read_resource(resource, boundary - 32, 32)
            assert peare_lib.read_resource(resource, boundary - 32, 64) == before
            with pytest.raises(RuntimeError):
                peare_lib.read_resource(resource, boundary, 512)
            assert peare_lib.read_resource(resource, 0, 512)[3:11] == b"NTFS    "
            assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
            with peare_lib._open_source(source) as ntfs:
                expected = {
                    "boot.ini": "e1e05ba3889a8a1f247a4d848dd34204fdb9964a4648b022430e5dcf98a70924",
                    "NTDETECT.COM": "200892088e72851346ba2161b8c276af060a12e4968185dece549e6c2c8adc9b",
                    "ntldr": "c6953d351909cb9e5419e9e5cbf89b6c5749777cacf04b5e2d72b2aaa2a52963",
                }
                for name, digest in expected.items():
                    assert hashlib.sha256(ntfs.read_member_bytes(name)).hexdigest() == digest
        finally:
            if source:
                opener._L.peare_source_destroy(source)
            opener._L.peare_resource_destroy(resource)


def test_tib_cross_volume_wof_wim(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "wof-cross-volume-ab15-20261122" / "wof-cross-volume-valid.TIB"
    if not sample.is_file():
        pytest.skip("The independent cross-volume WOF TIB sample is not available")
    expected = {
        "Partition 1/WOFSample/hosts": "2d6bdfb341be3a6234b24742377f93aa7c7cfb0d9fd64efa9282c87852e57085",
        "Partition 1/WOFSample/lmhosts.sam": "000fe9c924b4d155477cad15b4cfd30616c37523b4b848d6ecbd003507a55edf",
        "Partition 1/WOFSample/networks": "21de93ed8293dbb9c53b59a5c1af04b1fd997cf7dfbd0ba5f21cb315d845b7a8",
        "Partition 1/WOFSample/protocol": "52cf86496f3859d0f3e58776eccff1d6589792004d2291d3b0ce8d7635be7278",
        "Partition 1/WOFSample/services": "c1723f7f29b224c42f26452c3efa8f80f6ee8500ee78513e0c0732ba55399f7d",
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=4)
        by_path = {row["path"]: row for row in rows}
        for path, digest in expected.items():
            assert path in by_path, path
            assert by_path[path]["sha256"] == digest, path
            assert _resource_hash(opener, path) == digest


def test_tib_cross_volume_wof_mbr_backing(peare_lib):
    from test_tibx_abi import _resource_hash

    sample = Path(SAMPLES) / "wof-cross-volume-mbr-ab15-20261122" / "wof-cross-volume-mbr-valid.TIB"
    if not sample.is_file():
        pytest.skip("The independent MBR-backed cross-volume WOF TIB sample is not available")
    expected = {
        "Partition 4/WofMbrSample/hosts": "2d6bdfb341be3a6234b24742377f93aa7c7cfb0d9fd64efa9282c87852e57085",
        "Partition 4/WofMbrSample/lmhosts.sam": "000fe9c924b4d155477cad15b4cfd30616c37523b4b848d6ecbd003507a55edf",
        "Partition 4/WofMbrSample/networks": "21de93ed8293dbb9c53b59a5c1af04b1fd997cf7dfbd0ba5f21cb315d845b7a8",
        "Partition 4/WofMbrSample/protocol": "52cf86496f3859d0f3e58776eccff1d6589792004d2291d3b0ce8d7635be7278",
        "Partition 4/WofMbrSample/services": "c1723f7f29b224c42f26452c3efa8f80f6ee8500ee78513e0c0732ba55399f7d",
    }
    with peare_lib.open_file(str(sample)) as opener:
        rows = opener.normalized_tree(max_depth=4)
        by_path = {row["path"]: row for row in rows}
        for path, digest in expected.items():
            assert path in by_path, path
            assert by_path[path]["sha256"] == digest, path
            assert _resource_hash(opener, path) == digest


def test_tib_sector_nested_files(peare_lib):
    sample = Path(SAMPLES) / "win2k-sp4.tib"
    if not sample.is_file():
        pytest.skip("The independent Windows 2000 TIB sample is not available")

    def open_child(opener, name, stack):
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = _HANDLE()
                assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(resource)) == 0
                try:
                    ctx = _Context()
                    assert opener._L.peare_resource_get_context(resource, C.byref(ctx)) == 0
                    actual = ctx.identifier_utf8.to_bytes().decode("utf-8")
                    opener._L.peare_resource_context_free(C.byref(ctx))
                    if actual.casefold() != name.casefold():
                        continue
                    source = _HANDLE()
                    assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
                    try:
                        child = opener._lib._open_source(source)
                        assert child is not None
                        return stack.enter_context(child)
                    finally:
                        opener._L.peare_source_destroy(source)
                finally:
                    opener._L.peare_resource_destroy(resource)
        pytest.fail("Directory not found: " + name)

    def check(opener, name, digest):
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = _HANDLE()
                assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(resource)) == 0
                try:
                    ctx = _Context()
                    assert opener._L.peare_resource_get_context(resource, C.byref(ctx)) == 0
                    actual = ctx.identifier_utf8.to_bytes().decode("utf-8")
                    opener._L.peare_resource_context_free(C.byref(ctx))
                    if actual.casefold() == name.casefold():
                        assert hashlib.sha256(opener.read_member_bytes(actual)).hexdigest() == digest
                        return
                finally:
                    opener._L.peare_resource_destroy(resource)
        pytest.fail("File not found: " + name)

    with peare_lib.open_file(str(sample)) as opener:
        with ExitStack() as stack:
            volume = open_child(opener, "Partition 1", stack)
            windows = open_child(volume, "WINNT", stack)
            check(windows, "explorer.exe", "e4331566515640dccbfb509b207be70353346225914d5d8cbba4aa4a5d31cf04")
            system = open_child(windows, "system32", stack)
            names = [
                "ADVAPI32.DLL", "ASYCFILT.DLL", "BASESRV.DLL", "BROWSEUI.DLL", "CDFVIEW.DLL", "CEWMDM.dll",
                "CRYPT32.DLL", "CRYPTDLG.DLL", "CRYPTNET.DLL", "CRYPTUI.DLL", "CSRSRV.DLL",
                "D3DCompiler_33.dll", "D3DCompiler_34.dll", "D3DCompiler_35.dll", "D3DCompiler_36.dll",
                "D3DCompiler_37.dll", "D3DCompiler_38.dll", "D3DCompiler_39.dll", "D3DCompiler_40.dll",
                "D3DCompiler_41.dll", "D3DCompiler_42.dll", "D3DX9_37.dll", "D3DX9_38.dll", "D3DX9_39.dll",
                "D3DX9_40.dll", "D3DX9_41.dll", "D3DX9_42.dll", "DANIM.DLL", "DComExt.dll", "DHCPCSVC.DLL",
                "DXTMSFT.DLL", "DXTRANS.DLL",
            ]
            aggregate = hashlib.sha256()
            for name in names:
                digest = hashlib.sha256(system.read_member_bytes(name)).digest()
                aggregate.update(name.encode("utf-8") + b"\0" + digest)
            assert aggregate.hexdigest() == "1791f47302c7b938307495c6a24ee2e06b214e5c1cafea8288ad82c1454cabb9"
            check(system, "ntoskrnl.exe", "84172dbd7e148dcf6e87c2e75e0c8716b3afcb6041b39802f63fd7fb67d7a152")
            check(system, "cmd.exe", "4b04da85216ddbd98fa194d71f3c3acf311ac69fbe93444d9bc4c5464177762e")
            check(system, "ntdll.dll", "92bf49d01d528642c678b5704456a54b85e9c09aa2f5e1d594227605f4c13048")
            kernel = open_child(system, "ntoskrnl.exe", stack)
            sections = Counter(row["path"] for row in kernel.normalized_tree(max_depth=0, recurse_containers=False))
            for section in ("PAGE", "PAGEKD", "PAGELK", "PAGEVRFY"):
                assert sections[section] == 2
            drivers = open_child(system, "drivers", stack)
            check(drivers, "atapi.sys", "57c342b828f192374085c57756cb1286cbb60450c01051c45128839553f60138")
            check(drivers, "ntfs.sys", "20f180f0d349727b504541af4ff00d3091c83b479a70f8b7ef78700604de7e30")
            etc = open_child(drivers, "etc", stack)
            check(etc, "hosts", "f16c19748cefd356850be752ae3f4d30826ee9391e2d5e13cf628aeb7318b9fd")


def test_tib_legacy_compressed_ext_volumes(peare_lib):
    from test_tibx_abi import _resource_hash

    path = Path(SAMPLES) / "xandros-rus.tib"
    if not path.is_file():
        pytest.skip("The independent Xandros TIB sample is not available")
    assert path.stat().st_size == 1027047029
    with peare_lib.open_file(str(path)) as opener:
        assert opener.container_format() == "TIB"
        roots = opener.normalized_tree(max_depth=0, recurse_containers=False)
        assert [(row["path"], row["size"]) for row in roots] == [
            ("Partition 1", 2467551744), ("Partition 2", 1513451520),
            ("Partition 3", 8225280), ("Partition 4", 8225280),
        ]
        assert roots[0]["format"] == roots[1]["format"] == "EXT"
        assert roots[2]["format"] == "FAT" and roots[2]["is_container"]
        assert roots[3]["sha256"] == "ec89dcd0c6ac6834b9300c4c52d7d5fb7cc29c063b6d6cc7511c48462406f191"
        windows = [
            (0, 250000, "0198c03e411cbd1fea20169c922f3af37d27559c808b9933b49063dcf26ed60d"),
            (0, 262394000, "b5b010eaf0698627f37cf6f1865e6343429ef32701cfda24a4b0fc398944624e"),
            (0, 1048826000, "ad8d323bf8aedb66398317c7ba9ef9234339117d244b4c9b301c81e6b965ea85"),
            (1, 262394000, "f85f2c34eb2843d2aa5951ee6e8e76985655b2e3ae2cbdd76bdfd654ecf19997"),
        ]
        for index, offset, expected in windows:
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(opener._h, 0, index, C.byref(resource)) == 0
            try:
                data = peare_lib.read_resource(resource, offset, 16000)
                assert len(data) == 16000 and hashlib.sha256(data).hexdigest() == expected
            finally:
                opener._L.peare_resource_destroy(resource)
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 2, C.byref(resource)) == 0
        source = _HANDLE()
        try:
            boot = peare_lib.read_resource(resource, 0, 512)
            assert hashlib.sha256(boot).hexdigest() == "75f7975aaccb6d5657b136e2ff9998663d504cb56e5018fe1cdde88f46ab772e"
            assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
            with peare_lib._open_source(source) as fat:
                assert fat.container_format() == "FAT"
                assert fat.normalized_tree(max_depth=0) == []
        finally:
            if source:
                opener._L.peare_source_destroy(source)
            opener._L.peare_resource_destroy(resource)
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            with pytest.raises(RuntimeError):
                peare_lib.read_resource(resource, 9409 * 262144, 512)
        finally:
            opener._L.peare_resource_destroy(resource)
        for name, digest in [
            ("Partition 1/etc/issue", "46d4de5a40b8788abba1ae967a840c03fe59e0b2fc45fa737552b68d854d0ba1"),
            ("Partition 1/etc/fstab", "823db8e70578535b5fe7226b707287d268550bf92fc68dc0bea50c53fb704949"),
            ("Partition 1/etc/hostname", "c37a60f2fd40c4405bff1f14939af9b0bd19021d6aa5a8f8579f6d11cfedf104"),
            ("Partition 2/etc/hosts", "903b4c3ca69188a50008d0628a477fd7664e24669e081d91a5037cad0a7b241f"),
            ("Partition 2/etc/adjtime", "56becca59ca0be93513532a2c0372591219f99a52afc2227ece720b8c052e2a1"),
        ]:
            assert _resource_hash(opener, name) == digest


def test_tib_legacy_disk_and_files(peare_lib):
    path = Path(SAMPLES) / "lte-elite-hdd.tib"
    with peare_lib.open_file(str(path)) as opener:
        assert opener.container_format() == "TIB"
        roots = opener.normalized_tree(recurse_containers=False)
        assert [(row["path"], row["size"]) for row in roots] == [
            ("Partition 1", 337244160), ("Partition 3", 3356672)]
        assert all(row["is_container"] and row["format"] == "FAT" for row in roots)
        rows = opener.normalized_tree(max_depth=1)
        by_path = {row["path"]: row for row in rows}
        assert len(by_path) == len(rows)
        expected = {
            "Partition 1/AUTOEXEC.BAT": "15e3cc97a107832f44efc85f547c3a203f23fe8af9eec2765eb2bc2c34833bca",
            "Partition 1/MEM.CON": "801753cf20b4cfbce230cc42efd309a78f70871166bc5027092af0fefc53c24f",
            "Partition 1/NE.COM": "8f0c3534175cba8ea67bc2867a072db27168d31900736a42e457db9a9d03b795",
            "Partition 1/ANTI-VIR.DAT": "9aea0ac36ea4505179e0408525cf4e1a38cb895d30b5f7e2b7f69dff09093d31",
            "Partition 1/CCU3.LOG": "e84dbe7a5d875e7c0c5e140e3f37ac278f90f7253df26b3699a80bd6f474fd3b",
            "Partition 3/CONFIG.SYS": "06528bbac80ea87939e2f1a2b611cf154a06d03b3d659425ed222421a73bb64c",
        }
        for name, digest in expected.items():
            assert by_path[name]["sha256"] == digest, name


def test_tib_nested_in_zip(peare_lib):
    path = Path(SAMPLES) / "lte-elite-tib-container.zip"
    with peare_lib.open_file(str(path)) as opener:
        rows = opener.normalized_tree(max_depth=2)
        by_path = {row["path"]: row for row in rows}
        assert len(by_path) == len(rows)
        item = by_path["lte-elite-hdd.tib/Partition 1/AUTOEXEC.BAT"]
        assert item["sha256"] == "15e3cc97a107832f44efc85f547c3a203f23fe8af9eec2765eb2bc2c34833bca"


def test_samsung_tib_fat16_file(peare_lib):
    sample = Path(SAMPLES) / "samsung-svmi.tib"
    if not sample.is_file():
        pytest.skip("The independent Samsung TIB sample is not available")
    assert hashlib.sha256(sample.read_bytes()).hexdigest() == (
        "10c3546dbb251bf39c62e6b864839fc57502e95ca8202e782abfcda1a11e7bdd")
    with peare_lib.open_file(str(sample)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            source = _HANDLE()
            assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
            try:
                volume = peare_lib._open_source(source)
                assert volume is not None
                with volume:
                    data = volume.read_member_bytes("Autoexec.bat")
                    assert len(data) == 1635
                    assert hashlib.sha256(data).hexdigest() == (
                        "18c7288a12ec66457f37d1a39ca33c4c162c225098cadcd8042ee3f07ff756a7")
            finally:
                opener._L.peare_source_destroy(source)
        finally:
            opener._L.peare_resource_destroy(resource)


def test_samsung_tib_invalid_lfn_does_not_hide_short_name(peare_lib):
    sample = Path(SAMPLES) / "samsung-svmi.tib"
    if not sample.is_file():
        pytest.skip("The independent Samsung TIB sample is not available")
    with peare_lib.open_file(str(sample)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        source = _HANDLE()
        try:
            assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
            volume = peare_lib._open_source(source)
            assert volume is not None
            with volume:
                rows = volume.normalized_tree(max_depth=4, recurse_empty_containers_only=True)
        finally:
            if source:
                opener._L.peare_source_destroy(source)
            opener._L.peare_resource_destroy(resource)
        matches = [row for row in rows if row["path"].casefold() ==
                   "dta/activity.log"]
        assert len(matches) == 1
        assert matches[0]["size"] == 50922
        assert matches[0]["sha256"] == (
            "291cfb7bfff4638deb7ee22531057bc95e94ffb6ab157bd65520acf1656369f3")


def test_tib_legacy_read_failure_preserves_buffer(peare_lib):
    from test_callback_source_abi import CallbackSource, _find_resource
    from test_tibx_abi import _open_source_with_password

    path = Path(SAMPLES) / "lte-elite-hdd.tib"
    if not path.is_file():
        pytest.skip("The independent LTE Elite TIB sample is not available")

    class FailingSecondRead(CallbackSource):
        armed = False
        armed_calls = 0
        first_capacity = 0

        def read(self, context, offset, buffer, capacity, out_read):
            if self.armed:
                self.armed_calls += 1
                if self.armed_calls == 1:
                    self.first_capacity = capacity
                if self.armed_calls == 2:
                    return 13
            with self.lock:
                self.file.seek(offset)
                data = self.file.read(capacity)
                C.memmove(buffer, data, len(data))
                out_read[0] = len(data)
                return 0

    source = FailingSecondRead(peare_lib, path)
    opener = None
    resource = _HANDLE()
    try:
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and opener is not None
        resource, _ = _find_resource(opener, "Partition 1")
        source.armed = True
        length = 512 * 1024
        data = (C.c_uint8 * length)(*([0xa5] * length))
        count = C.c_size_t(1)
        status = opener._L.peare_resource_read_original(
            resource, 10 * 1024 * 1024, data, length, C.byref(count))
        assert source.armed_calls >= 2
        assert source.first_capacity >= 256 * 1024
        assert status != 0 and count.value == 0
        assert bytes(data) == bytes([0xa5]) * length
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if opener:
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@pytest.mark.parametrize(("index", "expected"), [
    (0, "151f0b65bbd4681ae5e4897c72404dbc6f63f1201c30073afab822061f480455"),
    (1, "4fde46e89bd5785958aca15f774138b0999c1f3166971e93722db752e896c79f"),
])
def test_tib_legacy_partition_bytes(peare_lib, index, expected):
    with peare_lib.open_file(str(Path(SAMPLES) / "lte-elite-hdd.tib")) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, index, C.byref(resource)) == 0
        try:
            size = [337244160, 3356672][index]
            digest = hashlib.sha256()
            for offset in range(0, size, 1024 * 1024):
                data = opener._lib.read_resource(resource, offset, min(1024 * 1024, size - offset))
                assert data
                digest.update(data)
            assert digest.hexdigest() == expected
        finally:
            opener._L.peare_resource_destroy(resource)


@pytest.mark.parametrize(("sample", "path", "expected"), [
    ("c_intel_full_b1_s1_v1.tib", "C:/Intel/Logs/IntelGFXCoin.log", "a4bd62c527d911eaf4a8cea56643b432a842555d16dc60ab7df33a0478fb6139"),
    ("c_intel_fullcompression_full_b1_s1_v1.tib", "C:/Intel/Logs/IntelGFXCoin.log", "a4bd62c527d911eaf4a8cea56643b432a842555d16dc60ab7df33a0478fb6139"),
    ("c_intel_fullcompression_full_b2_s1_v1.tib", "C:/Intel/Logs/IntelGFXCoin.log", "a4bd62c527d911eaf4a8cea56643b432a842555d16dc60ab7df33a0478fb6139"),
    ("testAcronis_full_b1_s1_v1.tib", "C:/testAcronis/100MB.bin:Zone.Identifier:$DATA", "f870e553a2c6289e1ee41c5d06eb49c0ba2efcdeeee0dccfc37dc589da561bd3"),
    ("win2k-sp4.tib", "Partition 1/boot.ini", "e1e05ba3889a8a1f247a4d848dd34204fdb9964a4648b022430e5dcf98a70924"),
    ("win2k-sp4.tib", "Partition 1/WINNT/system32/drivers/etc/hosts", "f16c19748cefd356850be752ae3f4d30826ee9391e2d5e13cf628aeb7318b9fd"),
    ("lte-elite-hdd.tib", "Partition 1/AUTOEXEC.BAT", "15e3cc97a107832f44efc85f547c3a203f23fe8af9eec2765eb2bc2c34833bca"),
    ("lte-elite-hdd.tib", "Partition 3/CONFIG.SYS", "06528bbac80ea87939e2f1a2b611cf154a06d03b3d659425ed222421a73bb64c"),
    ("samsung-svmi.tib", "Partition 1/Autoexec.bat", "18c7288a12ec66457f37d1a39ca33c4c162c225098cadcd8042ee3f07ff756a7"),
    ("lte-elite-tib-container.zip", "lte-elite-hdd.tib/Partition 3/CONFIG.SYS", "06528bbac80ea87939e2f1a2b611cf154a06d03b3d659425ed222421a73bb64c"),
    ("acronis15-created/disksample.tib", "Partition 1/DATA/CHANGE.TXT", "dd5f008775356126a1a4c1e3522bc6a2d96458716785b22d2764e4af2b383765"),
    ("xandros-rus.tib", "Partition 1/etc/issue", "46d4de5a40b8788abba1ae967a840c03fe59e0b2fc45fa737552b68d854d0ba1"),
    ("xandros-rus.tib", "Partition 2/etc/hosts", "903b4c3ca69188a50008d0628a477fd7664e24669e081d91a5037cad0a7b241f"),
])
def test_tib_legacy_gui_preview(peare_gui_path, tmp_path, sample, path, expected):
    if sample == "win2k-sp4.tib" and not (Path(SAMPLES) / sample).is_file():
        pytest.skip("The independent Windows 2000 TIB sample is not available")
    if sample == "samsung-svmi.tib" and not (Path(SAMPLES) / sample).is_file():
        pytest.skip("The independent Samsung TIB sample is not available")
    if sample == "xandros-rus.tib" and not (Path(SAMPLES) / sample).is_file():
        pytest.skip("The independent Xandros TIB sample is not available")
    if sample == "xandros-rus.tib":
        assert (Path(SAMPLES) / sample).stat().st_size == 1027047029
    root = Path(__file__).resolve().parents[1]
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
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
        str(peare_gui_path), "--preview-resource", str(Path(SAMPLES) / sample),
        path, str(output),
    ], capture_output=True, text=True, env=environment, timeout=120, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"] and inspection["sha256"] == expected
    assert not inspection["duplicate_paths"]
    assert output.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
