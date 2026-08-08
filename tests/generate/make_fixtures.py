#!/usr/bin/env python3
"""Generate every committed test fixture from scratch, deterministically.

Run:  python tests/generate/make_fixtures.py

Every byte produced here is synthetic and authored in this file, so the fixtures
carry no third-party or copyrighted content (see tests/POLICY.md). The fixtures
are committed (frozen); CI runs against them and does NOT regenerate, which keeps
the tests deterministic. Regenerate only when you intend to change a fixture, and
review the resulting binary + expected-manifest diff.
"""

import io
import os
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # tests/
SECTOR = 512


# --------------------------------------------------------------------------- FAT12

def _fat12_set(fat: bytearray, cluster: int, value: int) -> None:
    idx = cluster + cluster // 2
    if cluster & 1:
        fat[idx] = (fat[idx] & 0x0F) | ((value << 4) & 0xF0)
        fat[idx + 1] = (value >> 4) & 0xFF
    else:
        fat[idx] = value & 0xFF
        fat[idx + 1] = (fat[idx + 1] & 0xF0) | ((value >> 8) & 0x0F)


def _short_name(name: str) -> bytes:
    base, _, ext = name.partition(".")
    return (base.upper()[:8].ljust(8) + ext.upper()[:3].ljust(3)).encode("ascii")


def make_fat12(files: dict[str, bytes]) -> bytes:
    """Minimal, valid FAT12 volume containing `files` in the root directory."""
    bps, spc, rsvd, nfat, root_ent, fat_sz = SECTOR, 1, 1, 2, 16, 1
    root_sectors = (root_ent * 32 + bps - 1) // bps
    data_start = rsvd + nfat * fat_sz + root_sectors

    fat = bytearray(fat_sz * bps)
    _fat12_set(fat, 0, 0xFF8)
    _fat12_set(fat, 1, 0xFFF)

    root = bytearray()
    clusters_used = 0
    cluster = 2
    data_blocks: list[tuple[int, bytes]] = []  # (first_cluster, padded_bytes)
    for name, content in files.items():
        n = max(1, (len(content) + bps - 1) // bps)
        first = cluster
        for k in range(n):
            nxt = 0xFFF if k == n - 1 else cluster + 1
            _fat12_set(fat, cluster, nxt)
            cluster += 1
        clusters_used += n
        padded = content + b"\x00" * (n * bps - len(content))
        data_blocks.append((first, padded))
        e = bytearray(32)
        e[0:11] = _short_name(name)
        e[11] = 0x20
        struct.pack_into("<H", e, 26, first)
        struct.pack_into("<I", e, 28, len(content))
        root += e

    total_sectors = data_start + clusters_used
    img = bytearray(total_sectors * bps)

    boot = bytearray(bps)
    boot[0:3] = bytes([0xEB, 0x3C, 0x90])
    boot[3:11] = b"PEARETST"
    struct.pack_into("<H", boot, 11, bps)
    boot[13] = spc
    struct.pack_into("<H", boot, 14, rsvd)
    boot[16] = nfat
    struct.pack_into("<H", boot, 17, root_ent)
    struct.pack_into("<H", boot, 19, total_sectors)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, fat_sz)
    struct.pack_into("<H", boot, 24, 18)
    struct.pack_into("<H", boot, 26, 2)
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x50454152)
    boot[43:54] = b"PEARE FAT  "
    boot[54:62] = b"FAT12   "
    boot[510], boot[511] = 0x55, 0xAA
    img[0:bps] = boot

    img[rsvd * bps:(rsvd + fat_sz) * bps] = fat
    img[(rsvd + fat_sz) * bps:(rsvd + 2 * fat_sz) * bps] = fat  # FAT copy
    root_off = (rsvd + nfat * fat_sz) * bps
    img[root_off:root_off + len(root)] = root
    for first, padded in data_blocks:
        off = (data_start + (first - 2)) * bps
        img[off:off + len(padded)] = padded
    return bytes(img)


# ---------------------------------------------------------------- raw disk + VMDK

def make_raw_disk(partition_image: bytes, start_lba: int = 2048) -> bytes:
    part_sectors = len(partition_image) // SECTOR
    total_sectors = start_lba + part_sectors
    disk = bytearray(total_sectors * SECTOR)
    mbr = bytearray(SECTOR)
    e = 0x1BE
    mbr[e + 0] = 0x00                     # not bootable
    mbr[e + 1:e + 4] = bytes([0xFE, 0xFF, 0xFF])   # CHS start (ignored, LBA used)
    mbr[e + 4] = 0x01                     # type 0x01 = FAT12
    mbr[e + 5:e + 8] = bytes([0xFE, 0xFF, 0xFF])   # CHS end
    struct.pack_into("<I", mbr, e + 8, start_lba)
    struct.pack_into("<I", mbr, e + 12, part_sectors)
    mbr[510], mbr[511] = 0x55, 0xAA
    disk[0:SECTOR] = mbr
    disk[start_lba * SECTOR:start_lba * SECTOR + len(partition_image)] = partition_image
    return bytes(disk)


def make_flat_vmdk(disk: bytes, flat_name: str) -> bytes:
    """monolithicFlat descriptor referencing a sibling raw extent (`flat_name`)."""
    sectors = len(disk) // SECTOR
    return (
        "# Disk DescriptorFile\n"
        "version=1\n"
        'encoding="UTF-8"\n'
        "CID=12345678\n"
        "parentCID=ffffffff\n"
        'createType="monolithicFlat"\n'
        "\n"
        "# Extent description\n"
        f'RW {sectors} FLAT "{flat_name}" 0\n'
        "\n"
        "# The Disk Data Base\n"
        "#DDB\n"
        'ddb.adapterType = "ide"\n'
        'ddb.virtualHWVersion = "4"\n'
    ).encode("ascii")


# --------------------------------------------------------------------------- ZIP

def make_zip() -> bytes:
    buf = io.BytesIO()
    fixed = (1980, 1, 1, 0, 0, 0)
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in (
            ("readme.txt", b"Peare ZIP fixture.\n" * 40),
            ("sub/data.bin", bytes((i * 37 + 11) & 0xFF for i in range(1000))),
        ):
            zi = zipfile.ZipInfo(name, date_time=fixed)
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, data)
    return buf.getvalue()


# ------------------------------------------------------------------------- driver

def write(rel: str, data: bytes) -> None:
    path = ROOT / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(f"  {rel}  ({len(data)} bytes)")


def main() -> None:
    print("Generating fixtures:")

    fat_files = {
        "HELLO.TXT": b"Hello, Peare FAT test.\n",
        "DATA.BIN": bytes((i * 13 + 7) & 0xFF for i in range(600)),
    }
    fat_img = make_fat12(fat_files)
    write("formats/fat/fixtures/fat12_basic.img", fat_img)

    write("formats/zip/fixtures/basic.zip", make_zip())

    disk = make_raw_disk(fat_img)
    write("formats/vmdk_nested/fixtures/disk-flat.vmdk", disk)
    write("formats/vmdk_nested/fixtures/disk.vmdk", make_flat_vmdk(disk, "disk-flat.vmdk"))

    print("Done. Review the fixture and expected-manifest diffs before committing.")


if __name__ == "__main__":
    main()
