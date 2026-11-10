import ctypes as C

import base64

import binascii

import hashlib

import bz2

import gzip

import json

import io

import os

from pathlib import Path, PurePosixPath

import re

import shutil

import struct

import subprocess

import sys

import tarfile

import threading

import time

import uuid

import zipfile

import zlib

import lzma

import zstandard

import pytest

import peare_opener as po

ROOT = Path(__file__).resolve().parent.parent

def _seven_zip_executable():
    configured = os.environ.get("PEARE_7ZIP_EXE")
    if configured and Path(configured).is_file():
        return configured
    executable = shutil.which("7z.exe") or shutil.which("7z") or shutil.which("7zz")
    if executable:
        return executable
    if os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        if candidate.is_file():
            return str(candidate)
    return None

def _seven_zip_version(executable):
    result = subprocess.run([executable], capture_output=True, text=True,
                            timeout=10, check=False)
    match = re.search(r"\b7-Zip (\d+)\.(\d+)",
                      result.stdout + result.stderr)
    return tuple(map(int, match.groups())) if match else None

def _winrar_executable():
    configured = os.environ.get("PEARE_WINRAR_EXE")
    if configured and Path(configured).is_file():
        return configured
    roots = [Path(os.environ.get("ProgramFiles", "C:/Program Files")),
             Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))]
    for root in roots:
        candidate = root / "WinRAR" / "WinRAR.exe"
        if candidate.is_file():
            return str(candidate)
    return None

def _bsdtar_executable():
    return shutil.which("bsdtar.exe") or shutil.which("bsdtar")

class _ProcessIoCounters(C.Structure):
    _fields_ = [(name, C.c_ulonglong) for name in (
        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

def _process_io_counters():
    if os.name != "nt":
        pytest.skip("Process I/O counters are available only on Windows")
    kernel32 = C.WinDLL("kernel32", use_last_error=True)
    get_io = kernel32.GetProcessIoCounters
    get_io.argtypes = (C.c_void_p, C.POINTER(_ProcessIoCounters))
    get_io.restype = C.c_int
    counters = _ProcessIoCounters()
    assert get_io(C.c_void_p(-1), C.byref(counters))
    return counters.ReadTransferCount, counters.WriteTransferCount

def _zip_raw_member_payload(archive_path, member_name):
    with zipfile.ZipFile(archive_path) as archive:
        info = archive.getinfo(member_name)
    with open(archive_path, "rb") as archive_file:
        archive_file.seek(info.header_offset)
        header = archive_file.read(30)
        assert header[:4] == bytes((0x50, 0x4b, 0x03, 0x04))
        name_length, extra_length = struct.unpack_from("<HH", header, 26)
        archive_file.seek(info.header_offset + 30 + name_length + extra_length)
        return archive_file.read(info.compress_size)

@pytest.fixture(params=(b"fat12", b"fat16", b"fat32", b"exfat", b"ntfs"))
def filesystem_id(request):
    return request.param

def _ntfs_samples_from_env():
    configured = os.environ.get("PEARE_NTFS_TEST_IMAGES", "")
    if not configured:
        configured = os.environ.get("PEARE_NTFS_TEST_IMAGE", "")
    samples = [Path(value.strip()) for value in configured.split(os.pathsep)
               if value.strip()]
    return samples or [None]


def _zip_shrink_samples_from_env():
    configured = os.environ.get("PEARE_ZIP_SHRINK_TEST_IMAGES", "")
    return [Path(value.strip()) for value in configured.split(os.pathsep)
            if value.strip()] or [None]


def _zip64_sample_from_env():
    configured = os.environ.get("PEARE_ZIP64_TEST_IMAGE", "")
    return Path(configured) if configured else None


def _zip64_large_samples_from_env():
    configured = os.environ.get("PEARE_ZIP64_LARGE_TEST_IMAGES", "")
    return [Path(value.strip()) for value in configured.split(os.pathsep)
            if value.strip()] or [None]

@pytest.mark.parametrize("format_id", ["vhd", "vhdx", "vmdk", "raw-disk"])
def test_supported_filesystems_are_advertised_for_partitioned_disk_creation(
        writer_lib, format_id):
    format_id_bytes = format_id.encode("ascii")
    count = writer_lib.peare_writer_create_filesystem_count(format_id_bytes)
    filesystems = (_WriterVariant * count)()
    assert count == 8
    assert all(writer_lib.peare_writer_get_create_filesystem(
        format_id_bytes, index, C.byref(filesystems[index])) == 0
        for index in range(count))
    assert {item.id for item in filesystems} == {
        b"fat12", b"fat16", b"fat32", b"exfat", b"ntfs", b"ext2", b"ext3",
        b"ext4"}

def test_lumia_ext4_add_file_on_disposable_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_EXT_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_TEST_IMAGE to a real EXT sample")
    sample = Path(sample)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    image_path = str(image).encode()
    assert writer_lib.peare_writer_can_add_files(image_path) == 1
    assert writer_lib.peare_writer_update_target_count(image_path) == 1
    capabilities = C.c_uint32()
    assert writer_lib.peare_writer_get_update_capabilities(
        image_path, 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f

    source = ROOT / "README.md"
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    entry_name = f"PEARE-{uuid.uuid4().hex[:12]}.TXT"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, image_path, 0) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), entry_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    opener = peare_lib.open_file(str(image))
    assert opener is not None
    found_hash = None
    try:
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = _HANDLE()
                if peare_lib._lib.peare_opener_open_resource_at(
                        opener._h, folder, index, C.byref(resource)) != 0:
                    continue
                try:
                    context = po._Context()
                    if peare_lib._lib.peare_resource_get_context(
                            resource, C.byref(context)) != 0:
                        continue
                    name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                    peare_lib._lib.peare_resource_context_free(C.byref(context))
                    if name != entry_name:
                        continue
                    payload = po._Blob()
                    assert peare_lib._lib.peare_resource_get_payload(
                        resource, C.byref(payload)) == 0
                    try:
                        found_hash = hashlib.sha256(payload.to_bytes()).hexdigest()
                    finally:
                        peare_lib._lib.peare_blob_free(C.byref(payload))
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)
    finally:
        opener.close()
    assert found_hash == source_hash
    assert _sha256_file(sample) == original_hash

def test_lumia_ext4_cud_on_disposable_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_EXT_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_TEST_IMAGE to a real EXT sample")
    sample = Path(sample)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    oracle_before = None
    if sleuthkit_bin and (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        oracle_before = _tsk_recover_ext(
            sleuthkit_bin, image, 0, tmp_path / "lumia-ext4-before")
    source = ROOT / "README.md"
    replacement = ROOT / "src/writer/formats/ExtVolumeUpdate.cpp"
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    token = uuid.uuid4().hex[:12].upper()
    directory = f"PEARE-{token}"
    removed_directory = f"PEARE-REMOVE-{token}"
    removed_nested_directory = removed_directory + "".join(
        f"/D{depth:03d}" for depth in range(128))
    removed_nested_file = f"{removed_nested_directory}/PAYLOAD.TXT"
    initial_name = f"{directory}/INITIAL.TXT"
    final_name = f"{directory}/FINAL.TXT"
    deleted_name = f"{directory}/DELETE.TXT"
    deleted_large_name = f"{directory}/DELETE-LARGE.TXT"

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, directory.encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), initial_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), initial_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, initial_name.encode(), final_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), final_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), deleted_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, deleted_name.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(replacement).encode(), deleted_large_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, deleted_large_name.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, removed_directory.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, removed_nested_directory.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), removed_nested_file.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, removed_directory.encode(), 1) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    rows = _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=2)
    by_path = {row["path"]: row for row in rows}
    assert by_path.get(final_name, {}).get("sha256") == source_hash
    assert initial_name not in by_path
    assert deleted_name not in by_path
    assert deleted_large_name not in by_path
    assert removed_directory not in by_path
    assert not any(path.startswith(removed_directory + "/") for path in by_path)
    if oracle_before is not None:
        oracle_after = _tsk_recover_ext(
            sleuthkit_bin, image, 0, tmp_path / "lumia-ext4-after")
        assert all(oracle_after.get(path) == digest
                   for path, digest in oracle_before.items())
        assert not any(path.casefold().startswith(
            (removed_directory + "/").casefold()) for path in oracle_after)
        oracle_final = next(path for path in oracle_after
                            if _fat_path_matches(path, final_name))
        assert oracle_after[oracle_final] == bytes.fromhex(source_hash)
        extstat = subprocess.run(
            [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
             "-o", "0", "-f", "ext", str(image)],
            capture_output=True, text=True, timeout=60)
        assert extstat.returncode == 0, extstat.stdout + extstat.stderr
        assert "File System Type: Ext4" in extstat.stdout
    assert _sha256_file(sample) == original_hash

def _ext_shared_samples():
    configured = os.environ.get("PEARE_EXT_SHARED_BLOCKS_TEST_IMAGES", "")
    if not configured:
        configured = os.environ.get("PEARE_EXT_SHARED_BLOCKS_TEST_IMAGE", "")
    return [Path(value) for value in configured.split(os.pathsep) if value] or [None]

@pytest.mark.parametrize("sample", _ext_shared_samples(),
                         ids=lambda value: value.name if value else "unconfigured")
def test_ext4_shared_blocks_are_unshared_before_cud(writer_lib, tmp_path, sample):
    e2fsck_value = os.environ.get("PEARE_E2FSCK")
    debugfs_value = os.environ.get("PEARE_DEBUGFS")
    if sample is None or not sample.is_file():
        pytest.skip("Set PEARE_EXT_SHARED_BLOCKS_TEST_IMAGE to an e2fsprogs sample")
    if not e2fsck_value or not Path(e2fsck_value).is_file() or \
            not debugfs_value or not Path(debugfs_value).is_file():
        pytest.skip("Set PEARE_E2FSCK and PEARE_DEBUGFS to e2fsprogs utilities")

    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")

    def dump_file(target, path):
        result = subprocess.run(
            [debugfs_value, "-R", f"dump /{path} {target}", str(image)],
            capture_output=True, text=True, timeout=30, env=env)
        assert result.returncode == 0, result.stdout + result.stderr

    before = {}
    for name in ("file1.txt", "file2.txt"):
        target = tmp_path / f"before-{name}"
        dump_file(target, name)
        before[name] = _sha256_file(target)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, b"SHAREDTEST") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(),
            b"SHAREDTEST/BEFORE.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(ROOT / "README.md").encode(),
            b"SHAREDTEST/BEFORE.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"SHAREDTEST/BEFORE.TXT", b"SHAREDTEST/AFTER.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"SHAREDTEST/AFTER.TXT", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    raw = image.read_bytes()[1024:2048]
    assert not (int.from_bytes(raw[100:104], "little") & 0x4000)
    block_numbers = []
    for name in ("file1.txt", "file2.txt"):
        stat = subprocess.run(
            [debugfs_value, "-R", f"stat /{name}", str(image)],
            capture_output=True, text=True, timeout=30, env=env)
        assert stat.returncode == 0, stat.stdout + stat.stderr
        extent = re.search(r"\(0\):(\d+)", stat.stdout)
        assert extent, stat.stdout
        block_numbers.append(int(extent.group(1)))
        target = tmp_path / f"after-{name}"
        dump_file(target, name)
        assert _sha256_file(target) == before[name]
    assert block_numbers[0] != block_numbers[1]
    checked = subprocess.run(
        [e2fsck_value, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash

def test_ext4_shared_blocks_without_space_fails_atomically(writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_SHARED_BLOCKS_FULL_TEST_IMAGE")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_EXT_SHARED_BLOCKS_FULL_TEST_IMAGE to an e2fsprogs sample")
    sample = Path(sample_value)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        result = writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0)
        assert result != 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == original_hash
    assert _sha256_file(sample) == original_hash

def _ext_quota_project_samples():
    configured = os.environ.get("PEARE_EXT_QUOTA_PROJECT_TEST_IMAGES", "")
    if not configured:
        configured = os.environ.get("PEARE_EXT_QUOTA_PROJECT_TEST_IMAGE", "")
    return [Path(value) for value in configured.split(os.pathsep) if value] or [None]

def _ext_quota_samples():
    configured = os.environ.get("PEARE_EXT_QUOTA_TEST_IMAGES", "")
    return [Path(value) for value in configured.split(os.pathsep) if value] or [None]

@pytest.mark.parametrize("sample", _ext_quota_samples(),
                         ids=lambda value: value.name if value else "unconfigured")
def test_ext4_user_group_quota_cud(writer_lib, tmp_path, sample):
    e2fsck_value = os.environ.get("PEARE_E2FSCK")
    if sample is None or not sample.is_file():
        pytest.skip("Set PEARE_EXT_QUOTA_TEST_IMAGES to e2fsprogs Ext samples")
    if not e2fsck_value or not Path(e2fsck_value).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")

    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")
    repaired = subprocess.run(
        [e2fsck_value, "-fy", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert repaired.returncode in (0, 1), repaired.stdout + repaired.stderr

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, b"QTEST") == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), b"QTEST/BEFORE.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(ROOT / "README.md").encode(), b"QTEST/BEFORE.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"QTEST/BEFORE.TXT", b"QTEST/AFTER.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"QTEST/AFTER.TXT", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked = subprocess.run(
        [e2fsck_value, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash

@pytest.mark.parametrize("sample", _ext_quota_project_samples(),
                         ids=lambda value: value.name if value else "unconfigured")
def test_ext4_project_quota_inheritance_cud(writer_lib, tmp_path, sample):
    e2fsck_value = os.environ.get("PEARE_E2FSCK")
    debugfs_value = os.environ.get("PEARE_DEBUGFS")
    if sample is None or not sample.is_file():
        pytest.skip("Set PEARE_EXT_QUOTA_PROJECT_TEST_IMAGE to an external Ext4 sample")
    if not e2fsck_value or not Path(e2fsck_value).is_file() or \
            not debugfs_value or not Path(debugfs_value).is_file():
        pytest.skip("Set PEARE_E2FSCK and PEARE_DEBUGFS to e2fsprogs utilities")

    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, b"PROJECT7/CHILD") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(),
            b"PROJECT7/CHILD/INHERITED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(ROOT / "src/writer/formats/ExtVolumeUpdate.cpp").encode(),
            b"PROJECT7/CHILD/INHERITED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"PROJECT7/CHILD/INHERITED.TXT",
            b"PROJECT7/CHILD/RENAMED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(),
            b"PROJECT7/CHILD/DELETE.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"PROJECT7/CHILD/DELETE.TXT", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, b"PROJECT7/CHILD/NESTED") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(),
            b"PROJECT7/CHILD/NESTED/NESTED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")
    checked = subprocess.run(
        [e2fsck_value, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    rename_writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(rename_writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            rename_writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(rename_writer)
        assert writer_lib.peare_writer_rename_entry(
            rename_writer, b"PROJECT7/CHILD/RENAMED.TXT",
            b"PROJECT7/CHILD/RENAMED-ONLY.TXT") == 0, \
            writer_lib.peare_writer_last_error(rename_writer)
        assert writer_lib.peare_writer_update_finish(rename_writer) == 0, \
            writer_lib.peare_writer_last_error(rename_writer)
    finally:
        writer_lib.peare_writer_destroy(rename_writer)
    checked = subprocess.run(
        [e2fsck_value, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    inode = subprocess.run(
        [debugfs_value, "-R",
         "stat /PROJECT7/CHILD/RENAMED-ONLY.TXT", str(image)],
        capture_output=True, text=True, timeout=30, env=env)
    assert inode.returncode == 0, inode.stdout + inode.stderr
    assert "Project:     7" in inode.stdout, inode.stdout
    nested_inode = subprocess.run(
        [debugfs_value, "-R",
         "stat /PROJECT7/CHILD/NESTED/NESTED.TXT", str(image)],
        capture_output=True, text=True, timeout=30, env=env)
    assert nested_inode.returncode == 0, nested_inode.stdout + nested_inode.stderr
    assert "Project:     7" in nested_inode.stdout, nested_inode.stdout
    directory = subprocess.run(
        [debugfs_value, "-R", "stat /PROJECT7/CHILD", str(image)],
        capture_output=True, text=True, timeout=30, env=env)
    assert directory.returncode == 0, directory.stdout + directory.stderr
    assert "Project:     7" in directory.stdout, directory.stdout
    flags = re.search(r"Flags: 0x([0-9a-fA-F]+)", directory.stdout)
    assert flags and int(flags.group(1), 16) & 0x20000000, directory.stdout
    assert _sha256_file(sample) == original_hash

def test_ext4_project_quota_same_directory_rename_latency(writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_QUOTA_PROJECT_TEST_IMAGE", "")
    e2fsck_value = os.environ.get("PEARE_E2FSCK", "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_EXT_QUOTA_PROJECT_TEST_IMAGE to an e2fsprogs sample")
    if not e2fsck_value or not Path(e2fsck_value).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")

    sample = Path(sample_value)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    latency_stem = f"Peare-Latency-{uuid.uuid4().hex.upper()}"
    current_name = f"PROJECT7/{latency_stem}.TXT".encode()
    env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(),
            current_name) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    measurements = []
    for run in range(3):
        next_name = f"PROJECT7/{latency_stem}-{run}.TXT".encode()
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            assert writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), 0) == 0, \
                writer_lib.peare_writer_last_error(writer)
            io_before = _process_io_counters() if os.name == "nt" else None
            started = time.perf_counter()
            result = writer_lib.peare_writer_rename_entry(
                writer, current_name, next_name)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before)) \
                if io_before is not None else None
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            assert writer_lib.peare_writer_update_finish(writer) == 0, \
                writer_lib.peare_writer_last_error(writer)
            measurements.append((rename_seconds, rename_io))
        finally:
            writer_lib.peare_writer_destroy(writer)
        current_name = next_name

    checked = subprocess.run(
        [e2fsck_value, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    print("Ext4 project-quota rename seconds and I/O:", measurements)
    assert max(row[0] for row in measurements) < 2.0
    assert _sha256_file(sample) == original_hash

def test_ext4_external_journal_cud_through_writer_abi(writer_lib, tmp_path):
    filesystem_value = os.environ.get("PEARE_EXT_JOURNAL_FILESYSTEM_IMAGE")
    journal_value = os.environ.get("PEARE_EXT_JOURNAL_DEVICE_IMAGE")
    e2fsck_value = os.environ.get("PEARE_E2FSCK")
    debugfs_value = os.environ.get("PEARE_DEBUGFS")
    if not filesystem_value or not journal_value:
        pytest.skip("Set PEARE_EXT_JOURNAL_FILESYSTEM_IMAGE and PEARE_EXT_JOURNAL_DEVICE_IMAGE")
    filesystem = Path(filesystem_value)
    journal = Path(journal_value)
    if not filesystem.is_file() or not journal.is_file():
        pytest.skip("External journal sample images are unavailable")
    if not e2fsck_value or not debugfs_value:
        pytest.skip("Set PEARE_E2FSCK and PEARE_DEBUGFS to e2fsprogs utilities")

    original_hashes = (_sha256_file(filesystem), _sha256_file(journal))
    image = tmp_path / "ext4-external-journal.img"
    with image.open("wb") as output:
        for source in (filesystem, journal):
            with source.open("rb") as input_file:
                shutil.copyfileobj(input_file, output)
    journal_offset = filesystem.stat().st_size
    journal_size = journal.stat().st_size
    image_path = str(image).encode()
    assert writer_lib.peare_writer_update_target_count(image_path) == 1

    writer_lib.peare_writer_update_begin_target_with_journal.argtypes = [
        _HANDLE, C.c_char_p, C.c_size_t, C.c_int64, C.c_int64]
    writer_lib.peare_writer_update_begin_target_with_journal.restype = C.c_int
    source = ROOT / "README.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target_with_journal(
            writer, image_path, 0, journal_offset, journal_size) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, b"EXTJOURNAL") == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"EXTJOURNAL/CREATED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"EXTJOURNAL/CREATED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"EXTJOURNAL/CREATED.TXT", b"EXTJOURNAL/RENAMED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"EXTJOURNAL/RENAMED.TXT", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    filesystem_output = tmp_path / "filesystem-after.img"
    journal_output = tmp_path / "journal-after.img"
    with image.open("rb") as input_file:
        with filesystem_output.open("wb") as output:
            remaining = journal_offset
            while remaining:
                chunk = input_file.read(min(1024 * 1024, remaining))
                assert chunk
                output.write(chunk)
                remaining -= len(chunk)
        input_file.seek(journal_offset)
        with journal_output.open("wb") as output:
            shutil.copyfileobj(input_file, output)
    env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")
    checked = subprocess.run(
        [e2fsck_value, "-fn", "-j", str(journal_output),
         str(filesystem_output)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    stat = subprocess.run(
        [debugfs_value, "-R", "stat /EXTJOURNAL", str(filesystem_output)],
        capture_output=True, text=True, timeout=30, env=env)
    assert stat.returncode == 0 and "Type: directory" in stat.stdout
    assert (_sha256_file(filesystem), _sha256_file(journal)) == original_hashes

@pytest.mark.parametrize("block_size,cluster_size", [(1024, 16384), (4096, 65536)])
def test_ext4_bigalloc_cud_passes_e2fsck(writer_lib, tmp_path,
                                        block_size, cluster_size):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")
    sample = tmp_path / f"ext4-bigalloc-{block_size}-{cluster_size}-sample.img"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b", str(block_size),
         "-C", str(cluster_size), "-O", "bigalloc", str(sample), "16384"],
        capture_output=True, text=True, timeout=60, env=env)
    assert created.returncode == 0, created.stdout + created.stderr
    seed = ROOT / "README.md"
    for command in (f'write "{seed}" /SEED.TXT', "mkdir /BIG"):
        seeded = subprocess.run(
            [debugfs, "-w", "-R", command, str(sample)], capture_output=True,
            text=True, timeout=60, env=env)
        assert seeded.returncode == 0, seeded.stdout + seeded.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=60, env=env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    image = tmp_path / f"ext4-bigalloc-{block_size}-{cluster_size}-work.img"
    source_hash = _copy_file_sha256(sample, image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(seed).encode(), b"SEED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, b"BIG/CHILD") == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(seed).encode(), b"BIG/CHILD/CREATED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(seed).encode(), b"BIG/CHILD/CREATED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"BIG/CHILD/CREATED.TXT", b"BIG/CHILD/RENAMED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"BIG/CHILD/RENAMED.TXT", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"BIG", 1) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    stats = subprocess.run(
        [debugfs, "-R", "stat /SEED.TXT", str(image)],
        capture_output=True, text=True, timeout=30, env=env)
    assert stats.returncode == 0 and "Type: regular" in stats.stdout
    raw_superblock = image.read_bytes()[1024:2048]
    assert int.from_bytes(raw_superblock[100:104], "little") & 0x0200
    assert _sha256_file(sample) == source_hash

@pytest.mark.parametrize("inode_flag", [0x00000010, 0x00000020, 0x00000001,
                                         0x00000004, 0x00000100,
                                         0x00000200, 0x00004000,
                                         0x00400000, 0x00800000,
                                         0x80000000])
@pytest.mark.parametrize("operation", ["replace", "rename", "delete"])
def test_ext_inode_flags_reject_unsupported_mutations_atomically(
        writer_lib, tmp_path, inode_flag, operation):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    tool_env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        tool_env["PATH"] = runtime + os.pathsep + tool_env.get("PATH", "")
    sample = tmp_path / "ext-inode-flag-sample.img"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b", "1024",
         str(sample), "2048"], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert created.returncode == 0, created.stdout + created.stderr
    seed = ROOT / "README.md"
    seeded = subprocess.run(
        [debugfs, "-w", "-R", f'write "{seed}" /LOCKED.TXT', str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert seeded.returncode == 0, seeded.stdout + seeded.stderr
    stat = subprocess.run(
        [debugfs, "-R", "stat /LOCKED.TXT", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    flags_match = re.search(r"Flags: 0x([0-9a-fA-F]+)", stat.stdout)
    assert stat.returncode == 0 and flags_match, stat.stdout + stat.stderr
    flags = int(flags_match.group(1), 16) | inode_flag
    flagged = subprocess.run(
        [debugfs, "-w", "-R",
         f"set_inode_field /LOCKED.TXT flags 0x{flags:08x}", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert flagged.returncode == 0, flagged.stdout + flagged.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    image = tmp_path / f"ext-inode-flag-{inode_flag:08x}-{operation}.img"
    source_hash = _copy_file_sha256(sample, image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        if operation == "replace":
            result = writer_lib.peare_writer_replace_file(
                writer, str(seed).encode(), b"LOCKED.TXT")
        elif operation == "rename":
            result = writer_lib.peare_writer_rename_entry(
                writer, b"LOCKED.TXT", b"RENAMED.TXT")
        else:
            result = writer_lib.peare_writer_delete_entry(
                writer, b"LOCKED.TXT", 0)
        assert result != 0, writer_lib.peare_writer_last_error(writer)
        writer_lib.peare_writer_abort(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == source_hash


@pytest.mark.parametrize("directory_flag", [0x00000010, 0x00000020])
def test_ext_protected_directory_rejects_child_creation_atomically(
        writer_lib, tmp_path, directory_flag):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    tool_env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        tool_env["PATH"] = runtime + os.pathsep + tool_env.get("PATH", "")
    sample = tmp_path / "ext-protected-directory-sample.img"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b", "1024",
         str(sample), "2048"], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert created.returncode == 0, created.stdout + created.stderr
    made = subprocess.run(
        [debugfs, "-w", "-R", "mkdir /LOCKED", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    stat = subprocess.run(
        [debugfs, "-R", "stat /LOCKED", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    flags_match = re.search(r"Flags: 0x([0-9a-fA-F]+)", stat.stdout)
    assert made.returncode == 0 and stat.returncode == 0 and flags_match, \
        made.stdout + made.stderr + stat.stdout + stat.stderr
    flags = int(flags_match.group(1), 16) | directory_flag
    flagged = subprocess.run(
        [debugfs, "-w", "-R",
         f"set_inode_field /LOCKED flags 0x{flags:08x}", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert flagged.returncode == 0, flagged.stdout + flagged.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    image = tmp_path / f"ext-protected-directory-{directory_flag:08x}.img"
    source_hash = _copy_file_sha256(sample, image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        result = writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), b"LOCKED/NEW.TXT")
        assert result != 0, writer_lib.peare_writer_last_error(writer)
        writer_lib.peare_writer_abort(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == source_hash


def test_ext_inode_inherited_flags_are_preserved_on_new_entries(
        writer_lib, tmp_path):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    tool_env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        tool_env["PATH"] = runtime + os.pathsep + tool_env.get("PATH", "")
    sample = tmp_path / "ext-inherited-flags-sample.img"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b", "1024",
         str(sample), "2048"], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert created.returncode == 0, created.stdout + created.stderr
    made = subprocess.run(
        [debugfs, "-w", "-R", "mkdir /FLAGDIR", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    stat = subprocess.run(
        [debugfs, "-R", "stat /FLAGDIR", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    flags_match = re.search(r"Flags: 0x([0-9a-fA-F]+)", stat.stdout)
    assert made.returncode == 0 and stat.returncode == 0 and flags_match, \
        made.stdout + made.stderr + stat.stdout + stat.stderr
    inherited = (0x00000002 | 0x00000008 | 0x00000040 | 0x00000080 |
                 0x00000400 | 0x00008000 | 0x00010000 | 0x02000000)
    topdir = 0x00020000
    flags = int(flags_match.group(1), 16) | inherited | topdir
    flagged = subprocess.run(
        [debugfs, "-w", "-R",
         f"set_inode_field /FLAGDIR flags 0x{flags:08x}", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert flagged.returncode == 0, flagged.stdout + flagged.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    image = tmp_path / "ext-inherited-flags-work.img"
    source_hash = _copy_file_sha256(sample, image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), b"FLAGDIR/CHILD.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, b"FLAGDIR/SUBDIR") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    file_stat = subprocess.run(
        [debugfs, "-R", "stat /FLAGDIR/CHILD.TXT", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    dir_stat = subprocess.run(
        [debugfs, "-R", "stat /FLAGDIR/SUBDIR", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    parent_stat = subprocess.run(
        [debugfs, "-R", "stat /FLAGDIR", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    file_flags = re.search(r"Flags: 0x([0-9a-fA-F]+)", file_stat.stdout)
    dir_flags = re.search(r"Flags: 0x([0-9a-fA-F]+)", dir_stat.stdout)
    parent_flags = re.search(r"Flags: 0x([0-9a-fA-F]+)", parent_stat.stdout)
    assert file_flags and dir_flags and parent_flags, (
        file_stat.stdout + file_stat.stderr + dir_stat.stdout + dir_stat.stderr +
        parent_stat.stdout + parent_stat.stderr)
    file_inherited = inherited & ~0x00010000
    assert int(file_flags.group(1), 16) & file_inherited == file_inherited
    assert int(dir_flags.group(1), 16) & inherited == inherited
    assert int(parent_flags.group(1), 16) & topdir
    assert not (int(dir_flags.group(1), 16) & topdir)
    assert _sha256_file(sample) == source_hash

def test_ext4_encrypt_feature_plaintext_cud_passes_e2fsck(
        writer_lib, tmp_path):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    env = os.environ.copy()
    env["TEMP"] = str(tmp_path)
    env["TMP"] = str(tmp_path)
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")
    image = tmp_path / "ext4-encrypt-plaintext-cud.img"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b", "1024", "-O",
         "encrypt", str(image), "8192"], capture_output=True, text=True,
        timeout=60, env=env)
    assert created.returncode == 0, created.stdout + created.stderr
    seed = ROOT / "README.md"
    for command in (f'write "{seed}" /SEED.TXT', "mkdir /SAFE"):
        seeded = subprocess.run(
            [debugfs, "-w", "-R", command, str(image)], capture_output=True,
            text=True, timeout=60, env=env)
        assert seeded.returncode == 0, seeded.stdout + seeded.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(seed).encode(), b"SEED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, b"SAFE/CHILD") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(seed).encode(), b"SAFE/CHILD/CREATED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"SAFE/CHILD/CREATED.TXT", b"SAFE/CHILD/RENAMED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"SAFE/CHILD/RENAMED.TXT", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr

def test_ext4_encrypt_directories_reject_cud_without_mutation(
        writer_lib, peare_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_ENCRYPT_SAMPLE_GZ")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not sample_value or not e2fsck or not debugfs:
        pytest.skip("Set PEARE_EXT_ENCRYPT_SAMPLE_GZ and e2fsprogs tools")
    sample = Path(sample_value)
    if any(not Path(tool).is_file() for tool in (sample, e2fsck, debugfs)):
        pytest.skip("The upstream fscrypt sample or e2fsprogs is unavailable")

    image = tmp_path / "e2fsprogs-f_bad_encryption.img"
    with gzip.open(sample, "rb") as source, image.open("wb") as destination:
        shutil.copyfileobj(source, destination)
    env = os.environ.copy()
    runtime = os.environ.get("PEARE_E2FSPROGS_RUNTIME")
    if runtime:
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")
    repaired = subprocess.run(
        [e2fsck, "-fy", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert repaired.returncode in (0, 1), repaired.stdout + repaired.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr
    directory = subprocess.run(
        [debugfs, "-R", "stat /edir2", str(image)], capture_output=True,
        text=True, timeout=30, env=env)
    assert directory.returncode == 0, directory.stdout + directory.stderr
    assert "Flags: 0x800" in directory.stdout
    assert "c (40)" in directory.stdout
    with peare_lib.open_file(str(image)) as opener:
        paths = {row["path"] for row in opener.normalized_tree()}
    assert "edir2" in paths
    assert any(path.startswith("edir2/") for path in paths)
    original_hash = _sha256_file(image)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        status = writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), b"edir2/UNSAFE.TXT")
        assert status != 0
        assert "encrypted" in writer_lib.peare_writer_last_error(
            writer).decode("utf-8").lower()
        assert writer_lib.peare_writer_update_finish(writer) != 0

        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        status = writer_lib.peare_writer_rename_entry(
            writer, b"edir2", b"RENAMED")
        assert status != 0
        assert "encrypted" in writer_lib.peare_writer_last_error(
            writer).decode("utf-8").lower()
        assert writer_lib.peare_writer_update_finish(writer) != 0

        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        status = writer_lib.peare_writer_delete_entry(writer, b"edir2", 1)
        assert status != 0
        assert "encrypted" in writer_lib.peare_writer_last_error(
            writer).decode("utf-8").lower()
        assert writer_lib.peare_writer_update_finish(writer) != 0
    finally:
        writer_lib.peare_writer_destroy(writer)

    assert _sha256_file(image) == original_hash
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=60, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr

@pytest.mark.parametrize("target_depth", [0, 1, 2, 3, 4, 5])
def test_ext4_extent_tree_depths_replace_on_disposable_copy(
        writer_lib, tmp_path, target_depth):
    sample_value = os.environ.get("PEARE_EXT4_DEPTH3_TEST_IMAGE")
    tool_bin = os.environ.get("PEARE_E2FSPROGS_BIN")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_EXT4_DEPTH3_TEST_IMAGE to the e2fsprogs sample")
    if not tool_bin or not (Path(tool_bin) / "debugfs.exe").is_file() or \
            not (Path(tool_bin) / "e2fsck.exe").is_file():
        pytest.skip("Set PEARE_E2FSPROGS_BIN to the e2fsprogs utility directory")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")

    sample = Path(sample_value)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    debugfs = Path(tool_bin) / "debugfs.exe"
    e2fsck = Path(tool_bin) / "e2fsck.exe"
    tool_env = os.environ.copy()
    runtime_bin = Path(tool_bin).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    before_stat = subprocess.run(
        [str(debugfs), "-R", "stat DEEP.EXT", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert before_stat.returncode == 0, before_stat.stdout + before_stat.stderr
    inode_match = re.search(r"Inode:\s*(\d+)", before_stat.stdout)
    assert inode_match, before_stat.stdout[:1000]
    inode_number = int(inode_match.group(1))
    source_depth, source_extents = _ext4_extent_tree_map(
        image, 0, inode_number)
    assert source_depth == 3
    assert len(source_extents) == 30000
    if target_depth < source_depth:
        retained_extents = {0: 4, 1: 5, 2: 337}[target_depth]
        punch_start = retained_extents * 2
        punch_result = subprocess.run(
            [str(debugfs), "-w", "-R",
             f"punch <{inode_number}> {punch_start} 59999", str(image)],
            capture_output=True, text=True, timeout=120, env=tool_env)
        assert punch_result.returncode == 0, (
            punch_result.stdout + punch_result.stderr)
        optimize = subprocess.run(
            [str(e2fsck), "-fy", "-E", "optimize_extents", str(image)],
            capture_output=True, text=True, timeout=180, env=tool_env)
        assert optimize.returncode == 0, optimize.stdout + optimize.stderr

    base_depth, base_extents = _ext4_extent_tree_map(image, 0, inode_number)
    expected_extents = {0: 4, 1: 5, 2: 337}.get(target_depth, 30000)
    assert base_depth == min(target_depth, source_depth)
    assert len(base_extents) == expected_extents
    first_logical, first_raw_length, first_physical = base_extents[0]
    assert first_logical == 0
    assert first_raw_length > 32768

    commands = [
        f"zap_block -p 165 {first_physical}",
        f"extent_open <{inode_number}>",
        "root",
        *(["down"] * base_depth),
        f"replace_node {first_logical} 1 {first_physical}",
    ]
    for _ in range(target_depth - base_depth):
        commands.extend(("root", "split_node"))
    commands.extend(("extent_close", "quit"))
    debugfs_script = tmp_path / f"ext4-depth-{target_depth}.debugfs"
    debugfs_script.write_text("\n".join(commands) + "\n", encoding="ascii")
    prepare_result = subprocess.run(
        [str(debugfs), "-w", "-f", str(debugfs_script), str(image)],
        capture_output=True, text=True, timeout=120, env=tool_env)
    assert prepare_result.returncode == 0, (
        prepare_result.stdout + prepare_result.stderr)
    assert "replace_node" in prepare_result.stdout
    tree_depth, extents = _ext4_extent_tree_map(image, 0, inode_number)
    assert tree_depth == target_depth
    assert len(extents) == expected_extents
    assert extents[0] == (first_logical, 1, first_physical)
    assert all(raw_length > 32768 for _, raw_length, _ in extents[1:])
    assert all(logical % 2 == 0 for logical, _, _ in extents)
    assert extents[-1][0] + (extents[-1][1] - 32768) < 60000
    before_check = subprocess.run(
        [str(e2fsck), "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert before_check.returncode == 0, before_check.stdout + before_check.stderr
    before_files = _tsk_recover_ext(
        sleuthkit_bin, image, 0, tmp_path / "ext4-depth3-before")
    assert "DEEP.EXT" in before_files
    before_payload = next(
        (tmp_path / "ext4-depth3-before").rglob("DEEP.EXT"))
    assert before_payload.stat().st_size > 0
    with before_payload.open("rb") as recovered_payload:
        assert recovered_payload.read(1024) == bytes([0xA5]) * 1024
        while True:
            chunk = recovered_payload.read(1024 * 1024)
            if not chunk:
                break
            assert not any(chunk)

    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    after_cud_check = subprocess.run(
        [str(e2fsck), "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert after_cud_check.returncode == 0, (
        after_cud_check.stdout + after_cud_check.stderr)
    after_cud_stat = subprocess.run(
        [str(debugfs), "-R", "stat DEEP.EXT", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert after_cud_stat.returncode == 0, (
        after_cud_stat.stdout + after_cud_stat.stderr)
    after_cud_depth, after_cud_extents = _ext4_extent_tree_map(
        image, 0, inode_number)
    assert after_cud_depth == target_depth
    assert after_cud_extents == extents
    after_cud_files = _tsk_recover_ext(
        sleuthkit_bin, image, 0, tmp_path / "ext4-depth3-after-cud")
    assert after_cud_files["DEEP.EXT"] == before_files["DEEP.EXT"]
    assert after_cud_files[cud["final_name"]] == hashlib.sha256(
        cud["replacement_payload"]).digest()
    assert all(after_cud_files.get(path) == digest
               for path, digest in before_files.items())

    source = ROOT / "README.md"
    source_hash = hashlib.sha256(source.read_bytes()).digest()
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_target_count(str(image).encode()) == 1
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"DEEP.EXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    after_check = subprocess.run(
        [str(e2fsck), "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert after_check.returncode == 0, after_check.stdout + after_check.stderr
    after_stat = subprocess.run(
        [str(debugfs), "-R", "stat DEEP.EXT", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert after_stat.returncode == 0, after_stat.stdout + after_stat.stderr
    assert "(ETB" not in after_stat.stdout
    assert _ext4_extent_tree_map(image, 0, inode_number)[0] == 0
    after_files = _tsk_recover_ext(
        sleuthkit_bin, image, 0, tmp_path / "ext4-depth3-after")
    assert after_files["DEEP.EXT"] == source_hash
    assert after_files[cud["final_name"]] == hashlib.sha256(
        cud["replacement_payload"]).digest()
    assert all(after_files.get(path) == digest
               for path, digest in before_files.items() if path != "DEEP.EXT")
    assert _sha256_file(sample) == original_hash

@pytest.mark.parametrize("sample_env", [
    "PEARE_JFS_TEST_IMAGE",
    "PEARE_HPFS_TEST_IMAGE",
    "PEARE_QCOW2_TEST_IMAGE",
])
def test_read_only_disk_samples_expose_no_writer_target(writer_lib, sample_env):
    sample = os.environ.get(sample_env)
    if not sample or not Path(sample).is_file():
        pytest.skip(f"Set {sample_env} to the local read-only sample")
    image = Path(sample)
    source_hashes = {path: _sha256_file(path) for path in _disk_image_members(image)}
    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 0
    assert writer_lib.peare_writer_update_target_count(str(image).encode()) == 0
    assert {path: _sha256_file(path) for path in source_hashes} == source_hashes

def test_chr_ext2_cud_operations_on_disposable_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_EXT_DISK_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_DISK_TEST_IMAGE to the real CHR disk image")
    sample = Path(sample)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    image_path = str(image).encode()
    assert writer_lib.peare_writer_can_add_files(image_path) == 1
    assert writer_lib.peare_writer_update_target_count(image_path) == 2
    target_name = C.create_string_buffer(256)
    assert writer_lib.peare_writer_get_add_target_name(
        image_path, 0, target_name, len(target_name)) == 0
    assert b"Ext2" in target_name.value
    capabilities = C.c_uint32(0)
    assert writer_lib.peare_writer_get_update_capabilities(
        image_path, 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f

    original_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=0)
    }
    assert original_rows
    duplicate_name = next(iter(original_rows)).rsplit("/", 1)[-1]
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, duplicate_name)
    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    updated_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=1)
    }
    assert {path: updated_rows[path] for path in original_rows} == original_rows
    final_path = next(path for path in updated_rows
                      if _fat_path_matches(path, cud["final_name"]))
    assert updated_rows[final_path] == hashlib.sha256(
        cud["replacement_payload"]).hexdigest()
    assert _sha256_file(sample) == original_hash

@pytest.mark.parametrize(("sample_env", "expected_markers"), [
    ("PEARE_EXT_DISK_TEST_IMAGE", ("Ext2", "Ext3")),
    ("PEARE_CHR_7_20_7_TEST_IMAGE", ("FAT", "Ext4")),
])
def test_ext_preflight_accepts_real_samples_without_mutation(
        writer_lib, sample_env, expected_markers):
    sample_value = os.environ.get(sample_env)
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip(f"Set {sample_env} to its documented local sample")
    sample = Path(sample_value)
    original_hash = _sha256_file(sample)
    targets = _fat_writer_targets(writer_lib, sample)
    assert len(targets) == 2, targets
    for marker in expected_markers:
        assert any(marker in name for name in targets), targets
    for index, name in enumerate(targets):
        if not any(marker in name for marker in ("Ext2", "Ext3", "Ext4")):
            continue
        capabilities = C.c_uint32(0)
        assert writer_lib.peare_writer_get_update_capabilities(
            str(sample).encode(), index, C.byref(capabilities)) == 0
        assert capabilities.value == 0x3f, name
    assert _sha256_file(sample) == original_hash

def test_ext_geometry_cud_passes_e2fsck(writer_lib, tmp_path):
    samples = [Path(value) for value in os.environ.get(
        "PEARE_EXT_GEOMETRY_TEST_IMAGES", "").split(os.pathsep) if value]
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not samples or any(not sample.is_file() for sample in samples):
        pytest.skip("Set PEARE_EXT_GEOMETRY_TEST_IMAGES to external-tool Ext samples")
    expected_samples = {
        "ext2-rev0-block-1024.img",
        "ext2-rev1-block-2048.img",
        "ext2-rev1-block-4096.img",
        "ext2-block-65536-inode-32768.img",
        "ext3-block-1024.img",
        "ext3-block-2048.img",
        "ext3-block-4096.img",
        "ext3-block-65536-inode-32768.img",
        "ext3-65536-journal.img",
        "ext4-desc-32.img",
        "ext4-desc-64.img",
        "ext4-groups-64-flex-inode256.img",
        "ext4-groups-64-noflex-inode256.img",
        "ext4-groups-all-super.img",
        "ext4-groups-desc32-boundary-nojournal.img",
        "ext4-groups-desc32-boundary.img",
        "ext4-groups-desc64-boundary.img",
        "ext4-groups-flex-bg.img",
        "ext4-groups-meta-bg-boundary.img",
        "ext4-groups-meta-bg.img",
        "ext4-groups-sparse-super2.img",
        "ext4-groups-standard-desc32-64groups.img",
        "ext4-inode-128.img",
        "ext4-inode-512.img",
        "ext4-inode-1024.img",
    }
    expected_samples.update(f"ext2-{size}.img" for size in
                            (8192, 16384, 32768, 65536))
    expected_samples.update(f"ext3-{size}.img" for size in
                            (8192, 16384, 32768))
    expected_samples.update(f"ext4-block-{size}.img" for size in
                            (1024, 2048, 4096, 8192, 16384, 32768, 65536))
    expected_samples.update(f"ext4-block-65536-inode-{size}.img" for size in
                            (128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768))
    sample_names = {sample.name for sample in samples}
    assert len(samples) == len(expected_samples) and sample_names == expected_samples, (
        sorted(expected_samples - sample_names),
        sorted(sample_names - expected_samples),
    )
    if not e2fsck or not Path(e2fsck).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")
    if not debugfs or not Path(debugfs).is_file():
        pytest.skip("Set PEARE_DEBUGFS to the independent debugfs executable")

    for sample in samples:
        raw = sample.read_bytes()[1024:2048]
        incompat = int.from_bytes(raw[96:100], "little")
        read_only = int.from_bytes(raw[100:104], "little")
        descriptor_size = int.from_bytes(raw[254:256], "little") or 32
        if sample.name == "ext2-gdt-csum.img":
            assert read_only & 0x10
            assert not read_only & 0x400
            assert not incompat & 0x2000
        else:
            assert read_only & 0x400
            assert descriptor_size == (64 if sample.name == "ext4-metadata64.img" else 32)
            assert bool(incompat & 0x2000) == (sample.name == "ext4-seed32.img")
        image = tmp_path / sample.name
        original_hash = _copy_file_sha256(sample, image)
        image_path = str(image).encode()
        assert writer_lib.peare_writer_update_target_count(image_path) == 1
        target_name = C.create_string_buffer(256)
        assert writer_lib.peare_writer_get_update_target_name(
            image_path, 0, target_name, len(target_name)) == 0
        assert b"Ext" in target_name.value, target_name.value
        capabilities = C.c_uint32(0)
        assert writer_lib.peare_writer_get_update_capabilities(
            image_path, 0, C.byref(capabilities)) == 0
        assert capabilities.value == 0x3f

        cud = _fat_writer_cud(writer_lib, image, tmp_path)
        extracted = tmp_path / f"{sample.stem}-independent-check.bin"
        debug = subprocess.run(
            [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
            capture_output=True, text=True, timeout=120)
        assert debug.returncode == 0 and extracted.is_file(), \
            debug.stdout + debug.stderr
        assert extracted.read_bytes() == cud["replacement_payload"]
        checked = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        assert _sha256_file(sample) == original_hash

def test_ext_invalid_checksum_feature_interactions_fail_before_write(
        writer_lib, tmp_path):
    tool = os.environ.get("PEARE_DEBUGFS")
    sample_dir = os.environ.get("PEARE_EXT_CHECKSUM_SAMPLE_DIR")
    if not tool or not Path(tool).is_file() or not sample_dir:
        pytest.skip("Set PEARE_DEBUGFS and PEARE_EXT_CHECKSUM_SAMPLE_DIR")
    sample_dir = Path(sample_dir)
    profiles = (
        ("ext4-metadata64.img", "invalid-metadata-and-gdt-csum.img",
         "feature uninit_bg"),
        ("ext4-seed32.img", "invalid-seed-without-metadata-csum.img",
         "feature ^metadata_csum"),
    )
    runtime_bin = Path(tool).parent.parent / "bin"
    tool_env = os.environ.copy()
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    for source_name, invalid_name, command in profiles:
        image = tmp_path / invalid_name
        _copy_file_sha256(sample_dir / source_name, image)
        updated = subprocess.run(
            [tool, "-w", "-R", command, str(image)], capture_output=True,
            text=True, timeout=60, env=tool_env)
        assert updated.returncode == 0, updated.stdout + updated.stderr
        original_hash = _sha256_file(image)

        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            result = writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), 0)
            assert result != 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        assert _sha256_file(image) == original_hash

@pytest.mark.parametrize("sample_name", [
    "f_super_bad_csum.img",
    "j_corrupt_sb_magic.img",
    "j_corrupt_sb_csum.img",
    "j_corrupt_journal_block.img",
    "j_corrupt_commit_csum.img",
    "j_corrupt_descr_csum.img",
    "j_corrupt_revoke_csum.img",
])
def test_upstream_e2fsprogs_malformed_images_fail_before_write(
        writer_lib, tmp_path, sample_name):
    sample_dir = Path(os.environ.get("PEARE_E2FSPROGS_MALFORMED_SAMPLE_DIR", ""))
    sample = sample_dir / sample_name
    if not sample.is_file():
        pytest.skip("Set PEARE_E2FSPROGS_MALFORMED_SAMPLE_DIR to upstream fixtures")

    image = tmp_path / sample_name
    source_hash = _copy_file_sha256(sample, image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0)
        if status == 0:
            writer_lib.peare_writer_abort(writer)
        assert status != 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == source_hash
    assert _sha256_file(sample) == source_hash


@pytest.mark.parametrize("sample_name", [
    "f_bad_gdt_csum.img",
    "j_corrupt_commit_tid.img",
])
def test_upstream_e2fsprogs_ignored_errors_remain_checkable_after_cud(
        writer_lib, tmp_path, sample_name):
    sample_dir = Path(os.environ.get("PEARE_E2FSPROGS_MALFORMED_SAMPLE_DIR", ""))
    e2fsck = os.environ.get("PEARE_E2FSCK", "")
    sample = sample_dir / sample_name
    if not sample.is_file():
        pytest.skip("Set PEARE_E2FSPROGS_MALFORMED_SAMPLE_DIR to upstream fixtures")
    if not e2fsck or not Path(e2fsck).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")

    image = tmp_path / sample_name
    source_hash = _copy_file_sha256(sample, image)
    before = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120)
    assert before.returncode == 0, before.stdout + before.stderr
    _fat_writer_cud(writer_lib, image, tmp_path)
    after = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120)
    assert after.returncode == 0, after.stdout + after.stderr
    assert _sha256_file(sample) == source_hash


def test_jbd_invalid_superblocks_fail_before_write(writer_lib, tmp_path):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    sample_dir = os.environ.get("PEARE_EXT_CHECKSUM_SAMPLE_DIR")
    if not debugfs or not Path(debugfs).is_file() or not sample_dir:
        pytest.skip("Set PEARE_DEBUGFS and PEARE_EXT_CHECKSUM_SAMPLE_DIR")

    sample = Path(sample_dir) / "ext4-seed-journal-htree.img"
    if not sample.is_file():
        pytest.skip("The external e2fsprogs journal sample is not available")

    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    for case, journal_edits in (
            ("short-size", ("set_inode_field <8> size_lo 4096",)),
            ("zero-block-size", ("field:12:4:0x00",)),
            ("zero-first-block", ("field:20:4:0x00",)),
            ("start-out-of-range", ("field:28:4:0xff",)),
            ("maxlen-out-of-range", ("field:16:4:0xff",)),
            ("unknown-incompat", ("mask:0x80",)),
            ("unknown-rocompat", ("ro-mask:0x80",)),
            ("mutually-exclusive-checksums", ("mask:0x18",)),
            ("async-without-checksum", ("mask:0x04",)),
            ("invalid-checksum-type", ("mask:0x08", "type:0x01"))):
        image = tmp_path / f"invalid-jbd-{case}.img"
        _copy_file_sha256(sample, image)
        journal_block = None
        if any(edit.startswith(("mask:", "ro-mask:", "type:", "field:"))
               for edit in journal_edits):
            mapped = subprocess.run(
                [debugfs, "-R", "bmap <8> 0", str(image)],
                capture_output=True, text=True, timeout=60, env=tool_env)
            assert mapped.returncode == 0, mapped.stdout + mapped.stderr
            journal_block = mapped.stdout.strip().splitlines()[-1].strip()
            assert journal_block.isdecimal(), mapped.stdout

        for edit in journal_edits:
            if edit.startswith("mask:"):
                value = edit.partition(":")[2]
                command = f"zap_block -o 43 -l 1 -p {value} {journal_block}"
                if value == "0x80":
                    command = f"zap_block -o 40 -l 4 -p {value} {journal_block}"
            elif edit.startswith("ro-mask:"):
                value = edit.partition(":")[2]
                command = f"zap_block -o 47 -l 1 -p {value} {journal_block}"
            elif edit.startswith("type:"):
                value = edit.partition(":")[2]
                command = f"zap_block -o 80 -l 1 -p {value} {journal_block}"
            elif edit.startswith("field:"):
                _, offset, length, value = edit.split(":")
                command = (f"zap_block -o {offset} -l {length} -p "
                           f"{value} {journal_block}")
            else:
                command = edit
            changed = subprocess.run(
                [debugfs, "-w", "-R", command, str(image)],
                capture_output=True, text=True, timeout=60, env=tool_env)
            assert changed.returncode == 0, changed.stdout + changed.stderr

        malformed_hash = _sha256_file(image)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            result = writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), 0)
            if result == 0:
                writer_lib.peare_writer_abort(writer)
            assert result != 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        assert _sha256_file(image) == malformed_hash

@pytest.mark.parametrize(
    ("state", "recover"),
    [(0, False)] + [
        (state, recover)
        for state in range(2, 8)
        for recover in (False, True)
    ],
    ids=lambda value: str(value),
)
def test_ext_dirty_journal_states_fail_before_write(
        writer_lib, tmp_path, state, recover):
    sample_dir = os.environ.get("PEARE_EXT_CHECKSUM_SAMPLE_DIR")
    if not sample_dir:
        pytest.skip("Set PEARE_EXT_CHECKSUM_SAMPLE_DIR")
    sample = Path(sample_dir) / "ext4-seed-journal-htree.img"
    if not sample.is_file():
        pytest.skip("The external e2fsprogs journal sample is not available")

    image = tmp_path / f"ext-journal-state-{state}-{int(recover)}.img"
    original_hash = _copy_file_sha256(sample, image)
    with image.open("r+b") as target:
        target.seek(1024 + 58)
        target.write(state.to_bytes(2, "little"))
        target.seek(1024 + 96)
        incompat = int.from_bytes(target.read(4), "little")
        if recover:
            incompat |= 0x0004
        else:
            incompat &= ~0x0004
        target.seek(1024 + 96)
        target.write(incompat.to_bytes(4, "little"))

    malformed_hash = _sha256_file(image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        result = writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0)
        if result == 0:
            writer_lib.peare_writer_abort(writer)
        assert result != 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == malformed_hash
    assert _sha256_file(sample) == original_hash


@pytest.mark.parametrize(
    ("case", "field_offset", "value"),
    [
        ("external-journal", 228, (1).to_bytes(4, "little")),
        ("orphan-list", 232, (11).to_bytes(4, "little")),
    ],
)
def test_ext_unsupported_journal_references_fail_before_write(
        writer_lib, tmp_path, case, field_offset, value):
    sample_dir = os.environ.get("PEARE_EXT_CHECKSUM_SAMPLE_DIR")
    if not sample_dir:
        pytest.skip("Set PEARE_EXT_CHECKSUM_SAMPLE_DIR")
    sample = Path(sample_dir) / "ext4-seed-journal-htree.img"
    if not sample.is_file():
        pytest.skip("The external e2fsprogs journal sample is not available")

    image = tmp_path / f"ext-journal-{case}.img"
    original_hash = _copy_file_sha256(sample, image)
    with image.open("r+b") as target:
        target.seek(1024 + field_offset)
        target.write(value)
    malformed_hash = _sha256_file(image)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        result = writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0)
        if result == 0:
            writer_lib.peare_writer_abort(writer)
        assert result != 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == malformed_hash
    assert _sha256_file(sample) == original_hash


@pytest.mark.parametrize(
    "corruption", ("unknown-tag", "oversize-record", "bad-tail-checksum"))
def test_jbd_stale_fast_commit_corruption_is_ignored(
        writer_lib, tmp_path, corruption):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    sample_path = os.environ.get("PEARE_JBD_FAST_COMMIT_CRASH_RAW_SAMPLE")
    if (not debugfs or not Path(debugfs).is_file() or
            not e2fsck or not Path(e2fsck).is_file() or
            not sample_path or not Path(sample_path).is_file()):
        pytest.skip("Set the raw journal sample, PEARE_DEBUGFS, and PEARE_E2FSCK")

    sample = Path(sample_path)
    log = subprocess.run(
        [debugfs, "-R", "logdump", str(sample)], capture_output=True,
        text=True, timeout=60)
    assert log.returncode == 0, log.stdout + log.stderr
    commits = re.findall(
        r"Found expected sequence (\d+), type 2 \(commit block\)",
        log.stdout + log.stderr)
    heads = re.findall(r"tag HEAD, features 0x[0-9a-fA-F]+, tid (\d+)",
                       log.stdout + log.stderr)
    assert commits and heads, "Sample has no committed fast-commit record"
    assert int(heads[0]) != int(commits[-1]) + 1, \
        "Sample no longer has a stale fast-commit TID"

    image = tmp_path / f"fast-commit-{corruption}.raw"
    original_hash = _copy_file_sha256(sample, image)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    journal_dump = tmp_path / f"fast-commit-{corruption}-journal.raw"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump <8> {journal_dump}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and journal_dump.is_file(), \
        dumped.stdout + dumped.stderr
    journal = journal_dump.read_bytes()
    block_size = int.from_bytes(journal[12:16], "big")
    max_blocks = int.from_bytes(journal[16:20], "big")
    fast_commit_blocks = int.from_bytes(journal[84:88], "big")
    fast_commit_start = max_blocks - fast_commit_blocks
    assert block_size and fast_commit_blocks and fast_commit_start > 0

    records = []
    for block_index in range(fast_commit_blocks):
        block_offset = (fast_commit_start + block_index) * block_size
        offset = 0
        while offset + 4 <= block_size:
            tag, length = struct.unpack_from("<HH", journal, block_offset + offset)
            if not tag:
                break
            assert offset + 4 + length <= block_size
            records.append((block_index, offset, tag, length))
            if tag in (0x0007, 0x0008):
                break
            offset += 4 + length
    assert any(tag == 0x0009 for _, _, tag, _ in records)
    tail = next((record for record in records if record[2] == 0x0008), None)
    entry = next((record for record in records
                  if record[2] not in (0x0007, 0x0008, 0x0009)), None)
    assert tail and entry

    if corruption == "bad-tail-checksum":
        block_index, offset, _, _ = tail
        patch_offset = offset + 8
        patch_length = 1
        logical_byte = ((fast_commit_start + block_index) * block_size +
                        patch_offset)
        replacement = 0x00 if journal[logical_byte] else 0xff
    else:
        block_index, offset, _, _ = entry
        patch_offset = offset if corruption == "unknown-tag" else offset + 2
        patch_length = 2
        replacement = 0xff

    logical_block = fast_commit_start + block_index
    mapped = subprocess.run(
        [debugfs, "-R", f"bmap <8> {logical_block}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert mapped.returncode == 0, mapped.stdout + mapped.stderr
    physical_block = mapped.stdout.strip().splitlines()[-1].strip()
    assert physical_block.isdecimal(), mapped.stdout
    changed = subprocess.run(
        [debugfs, "-w", "-R",
         f"zap_block -o {patch_offset} -l {patch_length} -p "
         f"0x{replacement:02x} {physical_block}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert changed.returncode == 0, changed.stdout + changed.stderr
    malformed_hash = _sha256_file(image)

    assert _sha256_file(image) == malformed_hash
    _fat_writer_cud(writer_lib, image, tmp_path)
    check_env = os.environ.copy()
    check_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + \
        os.pathsep + check_env.get("PATH", "")
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=check_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash


@pytest.mark.parametrize(
    "corruption", ("unknown-tag", "oversize-record", "bad-tail-checksum"))
def test_jbd_active_fast_commit_corruption_fails_before_write(
        writer_lib, tmp_path, corruption):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    sample_path = os.environ.get("PEARE_JBD_FAST_COMMIT_VALID_RAW_SAMPLE")
    if (not debugfs or not Path(debugfs).is_file() or
            not sample_path or not Path(sample_path).is_file()):
        pytest.skip("Set the active fast-commit sample and debugfs")

    sample = Path(sample_path)
    original_hash = _sha256_file(sample)
    image = tmp_path / f"fast-commit-active-{corruption}.raw"
    _copy_file_sha256(sample, image)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    journal_path = tmp_path / f"fast-commit-active-{corruption}-journal.raw"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump <8> {journal_path}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and journal_path.is_file(), \
        dumped.stdout + dumped.stderr
    journal = journal_path.read_bytes()
    block_size = int.from_bytes(journal[12:16], "big")
    max_blocks = int.from_bytes(journal[16:20], "big")
    fast_commit_blocks = int.from_bytes(journal[84:88], "big")
    first_fc_block = max_blocks - fast_commit_blocks + 1
    assert block_size and fast_commit_blocks > 1 and first_fc_block > 0

    records = []
    for block_index in range(fast_commit_blocks - 1):
        block_offset = (first_fc_block + block_index) * block_size
        offset = 0
        while offset + 4 <= block_size:
            tag, length = struct.unpack_from(
                "<HH", journal, block_offset + offset)
            if not tag:
                break
            assert offset + 4 + length <= block_size
            records.append((block_index, offset, tag, length))
            if tag in (0x0007, 0x0008):
                break
            offset += 4 + length
    entry = next((record for record in records
                  if record[2] not in (0x0007, 0x0008, 0x0009)), None)
    tail = next((record for record in records if record[2] == 0x0008), None)
    assert entry and tail

    if corruption == "bad-tail-checksum":
        block_index, offset, _, _ = tail
        patch_offset = offset + 8
        logical_byte = ((first_fc_block + block_index) * block_size +
                        patch_offset)
        replacement = 0x00 if journal[logical_byte] else 0xff
        patch_length = 1
    else:
        block_index, offset, _, _ = entry
        patch_offset = offset if corruption == "unknown-tag" else offset + 2
        replacement = 0xff
        patch_length = 2

    logical_block = first_fc_block + block_index
    mapped = subprocess.run(
        [debugfs, "-R", f"bmap <8> {logical_block}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert mapped.returncode == 0, mapped.stdout + mapped.stderr
    physical_block = mapped.stdout.strip().splitlines()[-1].strip()
    assert physical_block.isdecimal(), mapped.stdout
    changed = subprocess.run(
        [debugfs, "-w", "-R",
         f"zap_block -o {patch_offset} -l {patch_length} -p "
         f"0x{replacement:02x} {physical_block}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert changed.returncode == 0, changed.stdout + changed.stderr
    corrupted_hash = _sha256_file(image)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        result = writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0)
        if result == 0:
            writer_lib.peare_writer_abort(writer)
        assert result != 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == corrupted_hash
    assert _sha256_file(sample) == original_hash


def test_jbd_fast_commit_clean_profile_cud_preserves_reserved_area(
        writer_lib, tmp_path):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    sample_path = os.environ.get("PEARE_JBD_FAST_COMMIT_CLEAN_SAMPLE")
    if (not debugfs or not Path(debugfs).is_file() or
            not e2fsck or not Path(e2fsck).is_file() or
            not sample_path or not Path(sample_path).is_file()):
        pytest.skip("Set the clean fast-commit sample and e2fsprogs tools")

    sample = Path(sample_path)
    image = tmp_path / "fast-commit-clean.img"
    original_hash = _copy_file_sha256(sample, image)
    superblock = image.read_bytes()[1024:2048]
    assert int.from_bytes(superblock[92:96], "little") & 0x400
    block_size = 1024 << int.from_bytes(superblock[24:28], "little")
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env = os.environ.copy()
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")

    clean_before = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert clean_before.returncode == 0, \
        clean_before.stdout + clean_before.stderr

    def read_journal(destination):
        result = subprocess.run(
            [debugfs, "-R", f"dump <8> {destination}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert result.returncode == 0 and destination.is_file(), \
            result.stdout + result.stderr
        data = destination.read_bytes()
        assert data[:4] == bytes.fromhex("c03b3998")
        reserved_blocks = int.from_bytes(data[0x54:0x58], "big")
        assert reserved_blocks == 128
        assert int.from_bytes(data[0x1C:0x20], "big") == 0
        return data[-reserved_blocks * block_size:]

    reserved_before = read_journal(tmp_path / "jbd-fast-commit-before.raw")
    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    reserved_after = read_journal(tmp_path / "jbd-fast-commit-after.raw")
    assert reserved_after == reserved_before

    extracted = tmp_path / "fast-commit-clean-check.bin"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and extracted.is_file(), \
        dumped.stdout + dumped.stderr
    assert extracted.read_bytes() == cud["replacement_payload"]
    clean_after = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert clean_after.returncode == 0, \
        clean_after.stdout + clean_after.stderr
    assert _sha256_file(sample) == original_hash


def test_jbd_active_fast_commit_replay_through_public_abi(
        writer_lib, tmp_path):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    sample_path = os.environ.get("PEARE_JBD_FAST_COMMIT_VALID_RAW_SAMPLE")
    if (not debugfs or not Path(debugfs).is_file() or
            not e2fsck or not Path(e2fsck).is_file() or
            not sample_path or not Path(sample_path).is_file()):
        pytest.skip("Set the active fast-commit sample and e2fsprogs tools")

    sample = Path(sample_path)
    original_hash = _sha256_file(sample)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")

    log = subprocess.run(
        [debugfs, "-R", "logdump", str(sample)], capture_output=True,
        text=True, timeout=60, env=tool_env)
    assert log.returncode == 0, log.stdout + log.stderr
    log_text = log.stdout + log.stderr
    commits = re.findall(
        r"Found expected sequence (\d+), type 2 \(commit block\)", log_text)
    heads = re.findall(r"tag HEAD, features 0x[0-9a-fA-F]+, tid (\d+)",
                       log_text)
    assert commits and heads, "Sample has no full commit followed by Fast Commit"
    assert int(heads[-1]) == int(commits[-1]) + 1, \
        "Fast Commit TID must immediately follow the last full commit"
    assert "tag ADD_RANGE" in log_text
    assert 'tag CREAT_DENTRY' in log_text and 'name "marker2' in log_text

    image = tmp_path / "fast-commit-active.raw"
    _copy_file_sha256(sample, image)
    _fat_writer_cud(writer_lib, image, tmp_path)

    for name in ("marker", "marker2"):
        extracted = tmp_path / f"fast-commit-{name}.bin"
        dumped = subprocess.run(
            [debugfs, "-R", f"dump /fc-lab/{name} {extracted}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert dumped.returncode == 0 and extracted.is_file(), \
            dumped.stdout + dumped.stderr
        assert extracted.read_bytes() == bytes(512)

    recovered_log = subprocess.run(
        [debugfs, "-R", "logdump", str(image)], capture_output=True,
        text=True, timeout=60, env=tool_env)
    assert recovered_log.returncode == 0, \
        recovered_log.stdout + recovered_log.stderr
    assert "Journal starts at block 0" in recovered_log.stdout
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash


def test_jbd_stale_fast_commit_vmdk_recovers_full_journal(
        writer_lib, peare_lib, tmp_path):
    sample_path = os.environ.get("PEARE_JBD_FAST_COMMIT_CRASH_SAMPLE")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    raw_sample_path = os.environ.get("PEARE_JBD_FAST_COMMIT_CRASH_RAW_SAMPLE")
    if (not sample_path or not Path(sample_path).is_file() or
            not debugfs or not Path(debugfs).is_file() or
            not raw_sample_path or not Path(raw_sample_path).is_file()):
        pytest.skip("Set the crash VMDK, its raw extent, and PEARE_DEBUGFS")

    log = subprocess.run(
        [debugfs, "-R", "logdump", raw_sample_path], capture_output=True,
        text=True, timeout=60)
    assert log.returncode == 0, log.stdout + log.stderr
    commits = re.findall(
        r"Found expected sequence (\d+), type 2 \(commit block\)",
        log.stdout + log.stderr)
    heads = re.findall(r"tag HEAD, features 0x[0-9a-fA-F]+, tid (\d+)",
                       log.stdout + log.stderr)
    assert commits and heads, "Sample has no committed fast-commit record"
    assert int(heads[0]) != int(commits[-1]) + 1, \
        "Sample no longer tests stale fast-commit data"

    sample = Path(sample_path)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)

    _fat_writer_cud(writer_lib, image, tmp_path)

    rows = _disk_filesystem_tree(peare_lib, image, "EXT")
    files = {
        row["path"].split("/", 1)[1]: row
        for row in rows
        if row["path"].startswith("Whole disk/") and not row["is_container"]
    }
    assert files["fc-fast-commit/hardlink.dat"]["sha256"] == \
        hashlib.sha256(b"overwrite-v2\n").hexdigest()
    assert files["fc-fast-commit/anchor.dat"]["sha256"] == \
        hashlib.sha256(b"anchor\n").hexdigest()
    assert "fc-fast-commit/file.dat" not in files
    assert "fc-fast-commit/renamed.dat" not in files
    assert _sha256_file(sample) == original_hash


@pytest.mark.parametrize(
    ("sample_env", "expected_marker", "committed"),
    [
        ("PEARE_JBD_FULL_COMMIT_SAMPLE",
         b"COMMITTED-BLOCK-V1\n", True),
        ("PEARE_JBD_INCOMPLETE_SAMPLE",
         b"independent legacy checksum v1 sample\n", False),
    ],
    ids=("committed-full-transaction", "incomplete-transaction"))
def test_jbd_full_transaction_replay_and_incomplete_discard(
        writer_lib, tmp_path, sample_env, expected_marker, committed):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    sample_path = os.environ.get(sample_env)
    if (not debugfs or not Path(debugfs).is_file() or
            not e2fsck or not Path(e2fsck).is_file() or
            not sample_path or not Path(sample_path).is_file()):
        pytest.skip(f"Set {sample_env}, PEARE_DEBUGFS, and PEARE_E2FSCK")

    sample = Path(sample_path)
    original_hash = _sha256_file(sample)
    image = tmp_path / sample.name
    shutil.copyfile(sample, image)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")

    before = subprocess.run(
        [debugfs, "-R", "logdump", str(image)], capture_output=True,
        text=True, timeout=60, env=tool_env)
    assert before.returncode == 0, before.stdout + before.stderr
    assert "type 1 (descriptor block)" in before.stdout
    if committed:
        assert "type 2 (commit block)" in before.stdout
    else:
        assert "type 2 (commit block)" not in before.stdout

    _fat_writer_cud(writer_lib, image, tmp_path)

    extracted = tmp_path / f"{sample.stem}-marker.txt"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump /marker.txt {extracted}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and extracted.is_file(), \
        dumped.stdout + dumped.stderr
    marker = extracted.read_bytes()
    if committed:
        assert marker.startswith(expected_marker)
        assert not any(marker[len(expected_marker):])
    else:
        assert marker == expected_marker

    after = subprocess.run(
        [debugfs, "-R", "logdump", str(image)], capture_output=True,
        text=True, timeout=60, env=tool_env)
    assert after.returncode == 0 and "Journal starts at block 0" in after.stdout
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash


def test_jbd_legacy_checksum_v1_cud_pass_e2fsck(writer_lib, tmp_path):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    sample_path = os.environ.get("PEARE_JBD_V1_SAMPLE")
    if (not debugfs or not Path(debugfs).is_file() or
            not e2fsck or not Path(e2fsck).is_file() or
            not sample_path or not Path(sample_path).is_file()):
        pytest.skip("Set the independent JBD v1 sample and e2fsprogs tools")

    sample = Path(sample_path)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    original_hash = _sha256_file(sample)
    info = subprocess.run(
        [str(Path(debugfs).with_name("dumpe2fs.exe")), "-h", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert info.returncode == 0, info.stdout + info.stderr
    assert "Journal features:         journal_checksum" in info.stdout
    assert "journal_checksum_v2" not in info.stdout
    assert "journal_checksum_v3" not in info.stdout

    for feature_mask in range(4):
        image = tmp_path / f"jbd-csum-v1-{feature_mask:02x}.img"
        shutil.copyfile(sample, image)
        mapped = subprocess.run(
            [debugfs, "-R", "bmap <8> 0", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert mapped.returncode == 0, mapped.stdout + mapped.stderr
        journal_block = mapped.stdout.strip().splitlines()[-1].strip()
        assert journal_block.isdecimal(), mapped.stdout
        changed = subprocess.run(
            [debugfs, "-w", "-R",
             f"zap_block -o 43 -l 1 -p 0x{feature_mask:02x} {journal_block}",
             str(image)], capture_output=True, text=True, timeout=60,
            env=tool_env)
        assert changed.returncode == 0, changed.stdout + changed.stderr

        clean_before = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert clean_before.returncode == 0, clean_before.stdout + clean_before.stderr

        cud = _fat_writer_cud(writer_lib, image, tmp_path)
        extracted = tmp_path / f"jbd-csum-v1-{feature_mask:02x}-check.bin"
        dumped = subprocess.run(
            [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert dumped.returncode == 0 and extracted.is_file(), \
            dumped.stdout + dumped.stderr
        assert extracted.read_bytes() == cud["replacement_payload"]

        clean_after = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert clean_after.returncode == 0, clean_after.stdout + clean_after.stderr
        assert _sha256_file(sample) == original_hash

def test_jbd_checksum_v2_cud_pass_e2fsck(writer_lib, tmp_path):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    sample_path = os.environ.get("PEARE_JBD_V2_SAMPLE")
    if (not debugfs or not Path(debugfs).is_file() or
            not e2fsck or not Path(e2fsck).is_file() or
            not sample_path or not Path(sample_path).is_file()):
        pytest.skip("Set the independent JBD v2 sample and e2fsprogs tools")

    sample = Path(sample_path)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    original_hash = _sha256_file(sample)
    info = subprocess.run(
        [str(Path(debugfs).with_name("dumpe2fs.exe")), "-h", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert info.returncode == 0, info.stdout + info.stderr
    assert "journal_checksum_v2" in info.stdout
    assert "journal_checksum_v3" not in info.stdout

    image = tmp_path / "jbd-csum-v2.img"
    shutil.copyfile(sample, image)
    clean_before = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert clean_before.returncode == 0, clean_before.stdout + clean_before.stderr

    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    extracted = tmp_path / "jbd-csum-v2-check.bin"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and extracted.is_file(), \
        dumped.stdout + dumped.stderr
    assert extracted.read_bytes() == cud["replacement_payload"]

    journal = tmp_path / "jbd-csum-v2-journal.raw"
    journal_dump = subprocess.run(
        [debugfs, "-R", f"dump <8> {journal}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert journal_dump.returncode == 0 and journal.is_file(), \
        journal_dump.stdout + journal_dump.stderr
    block_size_match = re.search(r"Block size:\s+(\d+)", info.stdout)
    assert block_size_match, info.stdout
    block_size = int(block_size_match.group(1))
    journal_bytes = journal.read_bytes()
    revoke_counts = []
    for offset in range(0, len(journal_bytes), block_size):
        block = journal_bytes[offset:offset + block_size]
        if len(block) != block_size or block[:8] != bytes.fromhex("c03b399800000005"):
            continue
        count = struct.unpack_from(">I", block, 12)[0]
        if (16 <= count <= block_size - 4 and
                (count - 16) % 4 == 0):
            revoke_counts.append(count)
    assert revoke_counts, "CUD journal contains no valid JBD2 revoke records"

    clean_after = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert clean_after.returncode == 0, clean_after.stdout + clean_after.stderr
    assert _sha256_file(sample) == original_hash

def test_jbd_v3_async_64bit_cud_pass_e2fsck(writer_lib, tmp_path):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    sample_path = os.environ.get("PEARE_JBD_V3_ASYNC_SAMPLE")
    if (not debugfs or not Path(debugfs).is_file() or
            not e2fsck or not Path(e2fsck).is_file() or
            not sample_path or not Path(sample_path).is_file()):
        pytest.skip("Set the independent JBD v3 async sample and e2fsprogs tools")

    sample = Path(sample_path)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    original_hash = _sha256_file(sample)
    info = subprocess.run(
        [str(Path(debugfs).with_name("dumpe2fs.exe")), "-h", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert info.returncode == 0, info.stdout + info.stderr
    assert "journal_64bit" in info.stdout
    assert "journal_async_commit" in info.stdout
    assert "journal_checksum_v3" in info.stdout

    image = tmp_path / "jbd-v3-async-64bit.img"
    shutil.copyfile(sample, image)
    clean_before = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert clean_before.returncode == 0, clean_before.stdout + clean_before.stderr

    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    extracted = tmp_path / "jbd-v3-async-check.bin"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and extracted.is_file(), \
        dumped.stdout + dumped.stderr
    assert extracted.read_bytes() == cud["replacement_payload"]
    clean_after = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert clean_after.returncode == 0, clean_after.stdout + clean_after.stderr
    assert _sha256_file(sample) == original_hash

def test_jbd_revoke_and_64bit_masks_cud_pass_e2fsck(
        writer_lib, tmp_path):
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    sample_dir = os.environ.get("PEARE_EXT_CHECKSUM_SAMPLE_DIR")
    if (not debugfs or not Path(debugfs).is_file() or
            not e2fsck or not Path(e2fsck).is_file() or not sample_dir):
        pytest.skip("Set PEARE_DEBUGFS, PEARE_E2FSCK, and the Ext sample directory")

    sample = Path(sample_dir) / "ext4-seed-journal-htree.img"
    if not sample.is_file():
        pytest.skip("The external e2fsprogs journal sample is not available")

    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    for feature_mask in (0x01, 0x02, 0x03):
        image = tmp_path / f"jbd-features-{feature_mask:02x}.img"
        original_hash = _copy_file_sha256(sample, image)
        mapped = subprocess.run(
            [debugfs, "-R", "bmap <8> 0", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert mapped.returncode == 0, mapped.stdout + mapped.stderr
        journal_block = mapped.stdout.strip().splitlines()[-1].strip()
        assert journal_block.isdecimal(), mapped.stdout
        changed = subprocess.run(
            [debugfs, "-w", "-R",
             f"zap_block -o 43 -l 1 -p 0x{feature_mask:02x} {journal_block}",
             str(image)], capture_output=True, text=True, timeout=60,
            env=tool_env)
        assert changed.returncode == 0, changed.stdout + changed.stderr

        clean_before = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert clean_before.returncode == 0, clean_before.stdout + clean_before.stderr

        cud = _fat_writer_cud(writer_lib, image, tmp_path)
        extracted = tmp_path / f"jbd-features-{feature_mask:02x}-check.bin"
        dumped = subprocess.run(
            [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert dumped.returncode == 0 and extracted.is_file(), \
            dumped.stdout + dumped.stderr
        assert extracted.read_bytes() == cud["replacement_payload"]
        clean_after = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert clean_after.returncode == 0, clean_after.stdout + clean_after.stderr
        assert _sha256_file(sample) == original_hash

def test_ext_checksum_layouts_cud_pass_e2fsck(writer_lib, tmp_path):
    samples = [Path(value) for value in os.environ.get(
        "PEARE_EXT_CHECKSUM_TEST_IMAGES", "").split(os.pathsep) if value]
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not samples or any(not sample.is_file() for sample in samples):
        pytest.skip("Set PEARE_EXT_CHECKSUM_TEST_IMAGES to external-tool Ext samples")
    expected_samples = {
        "ext2-gdt-csum.img",
        "ext3-gdt-csum.img",
        "ext4-gdt-csum.img",
        "ext4-metadata32.img",
        "ext4-metadata64.img",
        "ext4-seed32.img",
        "ext4-seed64.img",
        "ext4-seed-journal-htree.img",
    }
    sample_names = {sample.name for sample in samples}
    assert len(samples) == len(expected_samples) and sample_names == expected_samples, (
        sorted(expected_samples - sample_names),
        sorted(sample_names - expected_samples),
    )
    if not e2fsck or not Path(e2fsck).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")
    if not debugfs or not Path(debugfs).is_file():
        pytest.skip("Set PEARE_DEBUGFS to the independent debugfs executable")

    for sample in samples:
        raw = sample.read_bytes()[1024:2048]
        incompat = int.from_bytes(raw[96:100], "little")
        read_only = int.from_bytes(raw[100:104], "little")
        descriptor_size = int.from_bytes(raw[254:256], "little") or 32
        if sample.name.endswith("-gdt-csum.img"):
            assert read_only & 0x10
            assert not read_only & 0x400
            assert not incompat & 0x2000
        else:
            assert read_only & 0x400
            expected_desc_size = 64 if sample.name in {
                "ext4-metadata64.img", "ext4-seed64.img",
                "ext4-seed-journal-htree.img",
            } else 32
            assert descriptor_size == expected_desc_size
            assert bool(incompat & 0x2000) == sample.name.startswith("ext4-seed")
        if sample.name == "ext4-seed-journal-htree.img":
            assert int.from_bytes(raw[92:96], "little") & 0x4
            tree = subprocess.run(
                [debugfs, "-R", "htree_dump /MANY", str(sample)],
                capture_output=True, text=True, timeout=60)
            assert tree.returncode == 0 and "Root node dump:" in tree.stdout, \
                tree.stdout + tree.stderr
            assert "Not a hash-indexed directory" not in tree.stdout
        clean_before = subprocess.run(
            [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
            timeout=120)
        assert clean_before.returncode == 0, \
            clean_before.stdout + clean_before.stderr
        image = tmp_path / sample.name
        original_hash = _copy_file_sha256(sample, image)
        image_path = str(image).encode()
        assert writer_lib.peare_writer_update_target_count(image_path) == 1
        target_name = C.create_string_buffer(256)
        assert writer_lib.peare_writer_get_update_target_name(
            image_path, 0, target_name, len(target_name)) == 0
        assert b"Ext" in target_name.value, target_name.value
        capabilities = C.c_uint32(0)
        assert writer_lib.peare_writer_get_update_capabilities(
            image_path, 0, C.byref(capabilities)) == 0
        assert capabilities.value == 0x3f

        base_directory = "MANY" if sample.name == "ext4-seed-journal-htree.img" else ""
        cud = _fat_writer_cud(
            writer_lib, image, tmp_path, base_directory=base_directory)
        extracted = tmp_path / f"{sample.stem}-checksum-check.bin"
        debug = subprocess.run(
            [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
            capture_output=True, text=True, timeout=120)
        assert debug.returncode == 0 and extracted.is_file(), \
            debug.stdout + debug.stderr
        assert extracted.read_bytes() == cud["replacement_payload"]
        checked = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        assert _sha256_file(sample) == original_hash

def test_ext4_directory_hash_variants_cud_pass_e2fsck(writer_lib, tmp_path):
    sample_dir = os.environ.get("PEARE_EXT_HASH_TEST_DIR")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not sample_dir or not Path(sample_dir).is_dir():
        pytest.skip("Set PEARE_EXT_HASH_TEST_DIR to the external-tool samples")
    if not e2fsck or not Path(e2fsck).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")
    if not debugfs or not Path(debugfs).is_file():
        pytest.skip("Set PEARE_DEBUGFS to the independent debugfs executable")

    expected = {
        "ext4-hash-legacy-signed.img": ("legacy", "signed_directory_hash", 0),
        "ext4-hash-half_md4-signed.img": ("half_md4", "signed_directory_hash", 1),
        "ext4-hash-tea-signed.img": ("tea", "signed_directory_hash", 2),
        "ext4-hash-legacy-unsigned.img": ("legacy", "unsigned_directory_hash", 0),
        "ext4-hash-half_md4-unsigned.img": ("half_md4", "unsigned_directory_hash", 1),
        "ext4-hash-tea-unsigned.img": ("tea", "unsigned_directory_hash", 2),
    }
    samples = {name: Path(sample_dir) / name for name in expected}
    assert all(sample.is_file() for sample in samples.values()), samples
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    source = ROOT / "README.md"
    replacement = ROOT / "THIRDPARTY.md"
    replacement_payload = replacement.read_bytes()

    for name, sample in samples.items():
        algorithm, hash_flag, hash_version = expected[name]
        original_hash = _sha256_file(sample)
        info = subprocess.run(
            [debugfs, "-R", "show_super_stats", str(sample)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert info.returncode == 0, info.stdout + info.stderr
        assert f"Default directory hash:   {algorithm}" in info.stdout
        assert hash_flag in info.stdout
        tree = subprocess.run(
            [debugfs, "-R", "htree_dump /MANY", str(sample)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        version = re.search(r"Hash Version:\s*(\d+)", tree.stdout)
        assert tree.returncode == 0 and version and \
            int(version.group(1)) == hash_version, tree.stdout[:1000]
        checked = subprocess.run(
            [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert checked.returncode == 0, checked.stdout + checked.stderr

        image = tmp_path / name
        _copy_file_sha256(sample, image)
        cud = _fat_writer_cud(
            writer_lib, image, tmp_path, base_directory="MANY",
            source_file=source, replacement_file=replacement)
        extracted = tmp_path / f"{sample.stem}-hash-check.bin"
        dumped = subprocess.run(
            [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert dumped.returncode == 0 and extracted.is_file(), \
            dumped.stdout + dumped.stderr
        assert extracted.read_bytes() == replacement_payload
        checked = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        assert _sha256_file(sample) == original_hash

def test_ext2_legacy_directory_entries_cud_pass_e2fsck(writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_LEGACY_DIRENT_IMAGE")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_EXT_LEGACY_DIRENT_IMAGE to the external-tool sample")
    if not e2fsck or not Path(e2fsck).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")
    if not debugfs or not Path(debugfs).is_file():
        pytest.skip("Set PEARE_DEBUGFS to the independent debugfs executable")

    sample = Path(sample_value)
    raw = sample.read_bytes()[1024:2048]
    compat = int.from_bytes(raw[92:96], "little")
    incompat = int.from_bytes(raw[96:100], "little")
    assert not compat & 0x20
    assert not incompat & 0x2
    original_hash = _sha256_file(sample)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    clean = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert clean.returncode == 0, clean.stdout + clean.stderr

    image = tmp_path / sample.name
    _copy_file_sha256(sample, image)
    source = ROOT / "README.md"
    replacement = ROOT / "THIRDPARTY.md"
    cud = _fat_writer_cud(
        writer_lib, image, tmp_path, base_directory="MANY",
        source_file=source, replacement_file=replacement)
    extracted = tmp_path / "ext2-legacy-dirent-check.bin"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and extracted.is_file(), \
        dumped.stdout + dumped.stderr
    assert extracted.read_bytes() == cud["replacement_payload"]
    clean = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert clean.returncode == 0, clean.stdout + clean.stderr
    assert _sha256_file(sample) == original_hash

@pytest.mark.parametrize("filesystem", ["ext2", "ext3", "ext4"])
def test_ext_dir_prealloc_compatible_feature_cud_pass_e2fsck(
        writer_lib, tmp_path, filesystem):
    sample_value = os.environ.get(
        f"PEARE_EXT_DIR_PREALLOC_{filesystem.upper()}_IMAGE")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not sample_value or not e2fsck or not debugfs:
        pytest.skip("Set the DIR_PREALLOC sample and independent e2fsprogs tools")
    sample = Path(sample_value)
    if not sample.is_file() or not Path(e2fsck).is_file() or \
            not Path(debugfs).is_file():
        pytest.skip("The independent DIR_PREALLOC sample or tools are unavailable")

    original_hash = _sha256_file(sample)
    superblock = sample.read_bytes()[1024:2048]
    compat = int.from_bytes(superblock[92:96], "little")
    assert compat & 0x1
    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + os.pathsep + \
        tool_env.get("PATH", "")
    checked = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr

    image = tmp_path / sample.name
    _copy_file_sha256(sample, image)
    cud = _fat_writer_cud(
        writer_lib, image, tmp_path,
        source_file=ROOT / "README.md",
        replacement_file=ROOT / "THIRDPARTY.md")
    extracted = tmp_path / f"{filesystem}-dir-prealloc-check.md"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump /{cud['final_name']} {extracted}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and extracted.is_file(), \
        dumped.stdout + dumped.stderr
    assert extracted.read_bytes() == cud["replacement_payload"]
    stats = subprocess.run(
        [debugfs, "-R", "stats", str(image)], capture_output=True,
        text=True, timeout=60, env=tool_env)
    assert stats.returncode == 0 and "dir_prealloc" in stats.stdout, \
        stats.stdout + stats.stderr
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash

def test_ext_imagic_inodes_compatible_feature_cud_preserves_flagged_inode(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_IMAGIC_TEST_IMAGE")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not sample_value or not e2fsck or not debugfs:
        pytest.skip("Set the IMAGIC_INODES sample and independent e2fsprogs tools")
    sample = Path(sample_value)
    if not sample.is_file() or not Path(e2fsck).is_file() or \
            not Path(debugfs).is_file():
        pytest.skip("The independent IMAGIC_INODES sample or tools are unavailable")

    original_hash = _sha256_file(sample)
    superblock = sample.read_bytes()[1024:2048]
    compat = int.from_bytes(superblock[92:96], "little")
    assert compat & 0x2
    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + os.pathsep + \
        tool_env.get("PATH", "")
    checked = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    flagged_before = subprocess.run(
        [debugfs, "-R", "stat /bar", str(sample)], capture_output=True,
        text=True, timeout=60, env=tool_env)
    assert flagged_before.returncode == 0 and "Flags: 0x2000" in flagged_before.stdout, \
        flagged_before.stdout + flagged_before.stderr
    payload_before = tmp_path / "imagic-bar-before.bin"
    dumped_before = subprocess.run(
        [debugfs, "-R", f"dump /bar {payload_before}", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped_before.returncode == 0 and payload_before.is_file(), \
        dumped_before.stdout + dumped_before.stderr
    expected_payload = payload_before.read_bytes()

    image = tmp_path / sample.name
    _copy_file_sha256(sample, image)
    _fat_writer_cud(
        writer_lib, image, tmp_path,
        source_file=ROOT / "README.md",
        replacement_file=ROOT / "THIRDPARTY.md")
    flagged_after = subprocess.run(
        [debugfs, "-R", "stat /bar", str(image)], capture_output=True,
        text=True, timeout=60, env=tool_env)
    assert flagged_after.returncode == 0 and "Flags: 0x2000" in flagged_after.stdout, \
        flagged_after.stdout + flagged_after.stderr
    payload_after = tmp_path / "imagic-bar-after.bin"
    dumped_after = subprocess.run(
        [debugfs, "-R", f"dump /bar {payload_after}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped_after.returncode == 0 and payload_after.is_file(), \
        dumped_after.stdout + dumped_after.stderr
    assert payload_after.read_bytes() == expected_payload
    stats = subprocess.run(
        [debugfs, "-R", "stats", str(image)], capture_output=True,
        text=True, timeout=60, env=tool_env)
    assert stats.returncode == 0 and "imagic_inodes" in stats.stdout, \
        stats.stdout + stats.stderr
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash

@pytest.mark.parametrize("filesystem", ["ext2", "ext3", "ext4"])
def test_ext_resize_inode_compatible_feature_cud_preserves_reservations(
        writer_lib, tmp_path, filesystem):
    sample_value = os.environ.get(
        f"PEARE_EXT_RESIZE_INODE_{filesystem.upper()}_IMAGE")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not sample_value or not e2fsck or not debugfs:
        pytest.skip("Set the RESIZE_INODE sample and independent e2fsprogs tools")
    sample = Path(sample_value)
    if not sample.is_file() or not Path(e2fsck).is_file() or \
            not Path(debugfs).is_file():
        pytest.skip("The independent RESIZE_INODE sample or tools are unavailable")

    original_hash = _sha256_file(sample)
    superblock = sample.read_bytes()[1024:2048]
    compat = int.from_bytes(superblock[92:96], "little")
    read_only = int.from_bytes(superblock[100:104], "little")
    assert compat & 0x10 and read_only & 0x1
    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + os.pathsep + \
        tool_env.get("PATH", "")

    def debugfs_output(command, image):
        result = subprocess.run(
            [debugfs, "-R", command, str(image)], capture_output=True,
            text=True, timeout=60, env=tool_env)
        assert result.returncode == 0, result.stdout + result.stderr
        return "\n".join(line for line in result.stdout.splitlines()
                         if not line.startswith("debugfs ")).strip()

    checked = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    reserved_gdt_before = debugfs_output("stats", sample)
    reserved_gdt_before = next(
        line for line in reserved_gdt_before.splitlines()
        if line.strip().startswith("Reserved GDT blocks:"))
    inode_before = debugfs_output("stat <7>", sample)

    image = tmp_path / sample.name
    _copy_file_sha256(sample, image)
    _fat_writer_cud(
        writer_lib, image, tmp_path,
        source_file=ROOT / "README.md",
        replacement_file=ROOT / "THIRDPARTY.md")
    reserved_gdt_after = debugfs_output("stats", image)
    reserved_gdt_after = next(
        line for line in reserved_gdt_after.splitlines()
        if line.strip().startswith("Reserved GDT blocks:"))
    assert reserved_gdt_after == reserved_gdt_before
    assert debugfs_output("stat <7>", image) == inode_before
    stats = debugfs_output("stats", image)
    assert "resize_inode" in stats
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash

def test_ext4_stable_inodes_compatible_feature_cud_preserves_identity(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_STABLE_INODES_IMAGE")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not sample_value or not e2fsck or not debugfs:
        pytest.skip("Set the STABLE_INODES sample and independent e2fsprogs tools")
    sample = Path(sample_value)
    if not sample.is_file() or not Path(e2fsck).is_file() or \
            not Path(debugfs).is_file():
        pytest.skip("The independent STABLE_INODES sample or tools are unavailable")

    original_hash = _sha256_file(sample)
    source_bytes = sample.read_bytes()
    superblock = source_bytes[1024:2048]
    compat = int.from_bytes(superblock[92:96], "little")
    volume_uuid = superblock[104:120]
    assert compat & 0x0800
    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + os.pathsep + \
        tool_env.get("PATH", "")

    def debugfs_output(command, image):
        result = subprocess.run(
            [debugfs, "-R", command, str(image)], capture_output=True,
            text=True, timeout=60, env=tool_env)
        assert result.returncode == 0, result.stdout + result.stderr
        return "\n".join(line for line in result.stdout.splitlines()
                          if not line.startswith("debugfs ")).strip()

    checked = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    root_inode_before = debugfs_output("stat <2>", sample)
    seed_inode_before = debugfs_output("stat /seed", sample)

    image = tmp_path / sample.name
    _copy_file_sha256(sample, image)
    _fat_writer_cud(writer_lib, image, tmp_path)

    replacement = tmp_path / "stable-inodes-replacement.bin"
    replacement.write_bytes(b"Stable inode replacement payload\n")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"seed") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    superblock_after = image.read_bytes()[1024:2048]
    assert int.from_bytes(superblock_after[92:96], "little") & 0x0800
    assert superblock_after[104:120] == volume_uuid
    root_inode_after = debugfs_output("stat <2>", image)
    seed_inode_after = debugfs_output("stat /seed", image)
    assert root_inode_after.splitlines()[0] == root_inode_before.splitlines()[0]
    assert seed_inode_after.splitlines()[0] == seed_inode_before.splitlines()[0]
    payload_after = tmp_path / "stable-inodes-seed-after.bin"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump /seed {payload_after}", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and payload_after.is_file(), \
        dumped.stdout + dumped.stderr
    assert payload_after.read_bytes() == replacement.read_bytes()
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash

def test_ext_obsolete_lazy_bg_feature_fails_before_write(writer_lib):
    sample_value = os.environ.get("PEARE_EXT_LAZY_BG_PROBE_IMAGE")
    if not sample_value:
        pytest.skip("Set the external LAZY_BG probe image")
    sample = Path(sample_value)
    if not sample.is_file():
        pytest.skip("The external LAZY_BG probe image is unavailable")

    original_hash = _sha256_file(sample)
    superblock = sample.read_bytes()[1024:2048]
    assert int.from_bytes(superblock[92:96], "little") & 0x0040
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        result = writer_lib.peare_writer_update_begin_target(
            writer, str(sample).encode(), 0)
        if result == 0:
            writer_lib.peare_writer_abort(writer)
        assert result != 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(sample) == original_hash

def test_ext_snapshot_bitmap_feature_fails_before_write(writer_lib):
    sample_value = os.environ.get("PEARE_EXT_SNAPSHOT_BITMAP_PROBE_IMAGE")
    if not sample_value:
        pytest.skip("Set the external snapshot_bitmap probe image")
    sample = Path(sample_value)
    if not sample.is_file():
        pytest.skip("The external snapshot_bitmap probe image is unavailable")

    original_hash = _sha256_file(sample)
    superblock = sample.read_bytes()[1024:2048]
    assert int.from_bytes(superblock[92:96], "little") & 0x0100
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        result = writer_lib.peare_writer_update_begin_target(
            writer, str(sample).encode(), 0)
        if result == 0:
            writer_lib.peare_writer_abort(writer)
        assert result != 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(sample) == original_hash

def test_ext4_ea_inode_xattr_survives_cud_and_owner_delete(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_EA_INODE_IMAGE")
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not debugfs or not e2fsck:
        pytest.skip("Set PEARE_DEBUGFS and PEARE_E2FSCK")
    if not Path(debugfs).is_file() or not Path(e2fsck).is_file():
        pytest.skip("The independent e2fsprogs tools are unavailable")
    tool_env = os.environ.copy()
    tool_env["TEMP"] = str(tmp_path)
    tool_env["TMP"] = str(tmp_path)
    tool_root = Path(debugfs).parents[2]
    tool_env["PATH"] = os.pathsep.join((
        str(Path(debugfs).parent), str(tool_root / "usr" / "bin"),
        str(tool_root / "bin"), tool_env.get("PATH", "")))

    def run_debugfs(command, image):
        result = subprocess.run(
            [debugfs, "-R", command, str(image)], capture_output=True,
            text=True, timeout=60, env=tool_env)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout + result.stderr

    def check_e2fsck(image):
        result = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True,
            text=True, timeout=120, env=tool_env)
        assert result.returncode == 0, result.stdout + result.stderr

    if sample_value and Path(sample_value).is_file():
        sample = Path(sample_value)
    elif mke2fs and Path(mke2fs).is_file():
        sample = tmp_path / "ea-inode-mke2fs-sample.img"
        generated = subprocess.run(
            [mke2fs, "-F", "-q", "-t", "ext4", "-b", "4096", "-O",
             "ea_inode", str(sample), "8192"], capture_output=True,
            text=True, timeout=60, env=tool_env)
        assert generated.returncode == 0, generated.stdout + generated.stderr
        value_source = ROOT / "README.md"
        for command in (
                f'write "{value_source}" filler',
                f'write "{value_source}" seed',
                f'ea_set -f "{value_source}" seed user.large'):
            result = subprocess.run(
                [debugfs, "-w", "-R", command, str(sample)],
                capture_output=True, text=True, timeout=60, env=tool_env)
            assert result.returncode == 0, result.stdout + result.stderr
        repair = subprocess.run(
            [e2fsck, "-fy", str(sample)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert repair.returncode in (0, 1), repair.stdout + repair.stderr
        check_e2fsck(sample)
    else:
        pytest.skip("Set PEARE_MKE2FS or PEARE_EXT_EA_INODE_IMAGE")

    original_hash = _sha256_file(sample)
    superblock = sample.read_bytes()[1024:2048]
    assert int.from_bytes(superblock[96:100], "little") & 0x0400

    check_e2fsck(sample)
    image = tmp_path / "ea-inode-cud-copy.img"
    _copy_file_sha256(sample, image)
    value_before = tmp_path / "ea-inode-value-before.bin"
    run_debugfs(
        f'ea_get -f "{value_before}" seed user.large', image)
    assert value_before.is_file() and value_before.stat().st_size == 4096
    ea_inode_before = run_debugfs("stat <15>", image)
    assert "Flags: 0x280000" in ea_inode_before

    _fat_writer_cud(writer_lib, image, tmp_path)
    value_after_cud = tmp_path / "ea-inode-value-after-cud.bin"
    run_debugfs(
        f'ea_get -f "{value_after_cud}" seed user.large', image)
    assert value_after_cud.read_bytes() == value_before.read_bytes()
    check_e2fsck(image)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"seed", 0) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    check_e2fsck(image)
    assert _sha256_file(sample) == original_hash

@pytest.mark.parametrize("metadata_checksum", [True, False])
def test_ext4_mmp_guard_cud_passes_e2fsck(
        writer_lib, tmp_path, metadata_checksum):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")
    tool_env = os.environ.copy()
    tool_env["TEMP"] = str(tmp_path)
    tool_env["TMP"] = str(tmp_path)
    tool_root = Path(mke2fs).parents[2]
    tool_env["PATH"] = os.pathsep.join((
        str(Path(mke2fs).parent), str(tool_root / "usr" / "bin"),
        str(tool_root / "bin"), tool_env.get("PATH", "")))
    sample = tmp_path / f"mmp-feature-mke2fs-{int(metadata_checksum)}.img"
    features = "mmp" if metadata_checksum else "mmp,^metadata_csum"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b", "4096", "-O", features,
         "-E", "mmp_update_interval=1", str(sample), "8192"],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert created.returncode == 0, created.stdout + created.stderr
    raw = sample.read_bytes()[1024:2048]
    assert int.from_bytes(raw[96:100], "little") & 0x100
    assert bool(int.from_bytes(raw[100:104], "little") & 0x400) == metadata_checksum
    before_hash = _sha256_file(sample)
    image = tmp_path / "mmp-feature-cud-copy.img"
    _copy_file_sha256(sample, image)
    cud = _fat_writer_cud(writer_lib, image, tmp_path)

    check = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert check.returncode == 0, check.stdout + check.stderr
    mmp = subprocess.run(
        [debugfs, "-R", "dump_mmp", str(image)], capture_output=True,
        text=True, timeout=30, env=tool_env)
    assert mmp.returncode == 0, mmp.stdout + mmp.stderr
    assert "sequence: ff4d4d50" in (mmp.stdout + mmp.stderr).lower()
    extracted = tmp_path / "mmp-cud-result.bin"
    dump = subprocess.run(
        [debugfs, "-R", f'dump -p /{Path(cud["final_name"]).name} "{extracted}"',
         str(image)], capture_output=True, text=True, timeout=30, env=tool_env)
    assert dump.returncode == 0, dump.stdout + dump.stderr
    assert extracted.read_bytes() == cud["replacement_payload"]
    assert _sha256_file(sample) == before_hash

    active = tmp_path / f"mmp-active-{int(metadata_checksum)}.img"
    _copy_file_sha256(sample, active)
    active_tool = subprocess.Popen(
        [debugfs, "-w", str(active)], stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=tool_env)
    try:
        time.sleep(12)
        active_mmp = subprocess.run(
            [debugfs, "-R", "dump_mmp", str(active)], capture_output=True,
            text=True, timeout=30, env=tool_env)
        assert active_mmp.returncode == 0, active_mmp.stdout + active_mmp.stderr
        assert "sequence: e24d4d50" in (
            active_mmp.stdout + active_mmp.stderr).lower()
        active_hash = _sha256_file(active)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            result = writer_lib.peare_writer_update_begin_target(
                writer, str(active).encode(), 0)
            assert result != 0, "The MMP guard accepted an active FSCK sequence."
        finally:
            writer_lib.peare_writer_abort(writer)
            writer_lib.peare_writer_destroy(writer)
        assert _sha256_file(active) == active_hash
    finally:
        if active_tool.poll() is None:
            active_tool.stdin.write(b"quit\n")
            active_tool.stdin.flush()
            active_tool.wait(timeout=30)

@pytest.mark.parametrize("filesystem_block_size", [1024, 2048, 4096])
def test_ext4_verity_feature_volume_cud_passes_e2fsck(
        writer_lib, tmp_path, filesystem_block_size):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    tool_env = os.environ.copy()
    tool_env["TEMP"] = str(tmp_path)
    tool_env["TMP"] = str(tmp_path)
    tool_root = Path(mke2fs).parents[2]
    tool_env["PATH"] = os.pathsep.join((
        str(Path(mke2fs).parent), str(tool_root / "usr" / "bin"),
        str(tool_root / "bin"), tool_env.get("PATH", "")))
    sample = tmp_path / f"ext4-verity-{filesystem_block_size}.img"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b",
         str(filesystem_block_size), "-O", "verity", str(sample), "8192"],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert created.returncode == 0, created.stdout + created.stderr
    superblock = sample.read_bytes()[1024:2048]
    assert int.from_bytes(superblock[100:104], "little") & 0x8000
    baseline = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    image = tmp_path / "ext4-verity-feature-cud.img"
    source_hash = _copy_file_sha256(sample, image)
    token = uuid.uuid4().hex[:10].upper()
    directory = f"VERITY-{token}"
    initial = f"{directory}/INITIAL.TXT"
    renamed = f"{directory}/RENAMED.TXT"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, directory.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), initial.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(ROOT / "src/writer/formats/ExtVolumeUpdate.cpp").encode(),
            initial.encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, initial.encode(), renamed.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, renamed.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, directory.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert int.from_bytes(image.read_bytes()[1124:1128], "little") & 0x8000
    assert _sha256_file(sample) == source_hash

@pytest.mark.parametrize("filesystem_block_size", [1024, 2048, 4096])
def test_ext4_verity_inode_cow_rebuilds_merkle_tree(
        writer_lib, peare_lib, tmp_path, filesystem_block_size):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    tool_env = os.environ.copy()
    tool_env["TEMP"] = str(tmp_path)
    tool_env["TMP"] = str(tmp_path)
    tool_env["PATH"] = os.pathsep.join((
        str(Path(mke2fs).parent), str(Path(debugfs).parent),
        tool_env.get("PATH", "")))
    sample = tmp_path / f"ext4-verity-flagged-{filesystem_block_size}.img"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b",
         str(filesystem_block_size), "-O", "verity", str(sample), "8192"],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert created.returncode == 0, created.stdout + created.stderr
    seed = ROOT / "README.md"
    seeded = subprocess.run(
        [debugfs, "-w", "-R", f'write "{seed}" /PROTECTED.TXT',
         str(sample)], capture_output=True, text=True, timeout=60, env=tool_env)
    assert seeded.returncode == 0, seeded.stdout + seeded.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    image = tmp_path / "ext4-verity-inode-cud.img"
    source_hash = _copy_file_sha256(sample, image)
    replacement = ROOT / "src/writer/formats/ExtVolumeUpdate.cpp"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        status = writer_lib.peare_writer_enable_fsverity(
            writer, b"PROTECTED.TXT")
        error = writer_lib.peare_writer_last_error(writer).decode("utf-8")
        assert status == 0, error
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)

        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        status = writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"PROTECTED.TXT")
        error = writer_lib.peare_writer_last_error(writer).decode("utf-8")
        assert status == 0, error
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    extracted = tmp_path / "verity-flagged-payload.txt"
    dumped = subprocess.run(
        [debugfs, "-R", f'dump -p /PROTECTED.TXT "{extracted}"', str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0, dumped.stdout + dumped.stderr
    payload = replacement.read_bytes()
    assert extracted.read_bytes() == payload
    stat = subprocess.run(
        [debugfs, "-R", "stat /PROTECTED.TXT", str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert stat.returncode == 0, stat.stdout + stat.stderr
    assert "0x180000" in (stat.stdout + stat.stderr).lower()

    expected_tree, expected_descriptor = _fsverity_metadata(
        payload, filesystem_block_size)
    metadata_start = ((len(payload) + 65535) // 65536) * 65536
    descriptor_position = (
        (metadata_start + len(expected_tree) + filesystem_block_size - 1) //
        filesystem_block_size) * filesystem_block_size
    descriptor_size_position = (
        (descriptor_position + len(expected_descriptor) + 4 +
         filesystem_block_size - 1) // filesystem_block_size *
        filesystem_block_size - 4)
    assert _ext4_read_logical(
        debugfs, image, "/PROTECTED.TXT", metadata_start,
        len(expected_tree), filesystem_block_size, tool_env) == expected_tree
    assert _ext4_read_logical(
        debugfs, image, "/PROTECTED.TXT", descriptor_position,
        len(expected_descriptor), filesystem_block_size, tool_env) == \
        expected_descriptor
    assert _ext4_read_logical(
        debugfs, image, "/PROTECTED.TXT", descriptor_size_position,
        4, filesystem_block_size, tool_env) == struct.pack(
            "<I", len(expected_descriptor))
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    opener = peare_lib.open_file(str(image))
    assert opener is not None
    try:
        protected = next(row for row in opener.normalized_tree()
                         if row["path"] == "PROTECTED.TXT")
        assert protected["size"] == len(payload)
        assert protected["sha256"] == hashlib.sha256(payload).hexdigest()
    finally:
        opener.close()

    def protected_payload_status(path):
        candidate = peare_lib.open_file(str(path))
        assert candidate is not None
        try:
            api = peare_lib._lib
            for folder in range(candidate.folder_count()):
                for index in range(candidate.resource_count(folder)):
                    resource = _HANDLE()
                    assert api.peare_opener_open_resource_at(
                        candidate._h, folder, index, C.byref(resource)) == 0
                    try:
                        context = po._Context()
                        assert api.peare_resource_get_context(
                            resource, C.byref(context)) == 0
                        name = context.identifier_utf8.to_bytes().decode()
                        api.peare_resource_context_free(C.byref(context))
                        if name == "PROTECTED.TXT":
                            blob = po._Blob()
                            status = api.peare_resource_get_payload(
                                resource, C.byref(blob))
                            length = blob.length
                            api.peare_blob_free(C.byref(blob))
                            source = _HANDLE()
                            assert api.peare_resource_get_source(
                                resource, C.byref(source)) == 0
                            nested = _HANDLE()
                            assert api.peare_opener_create(C.byref(nested)) == 0
                            source_status = api.peare_opener_open(nested, source)
                            api.peare_opener_destroy(nested)
                            api.peare_source_destroy(source)
                            return status, length, source_status
                    finally:
                        api.peare_resource_destroy(resource)
            raise AssertionError("Protected verity file is missing from the Opener ABI")
        finally:
            candidate.close()

    corruption_blocks = (
        _ext4_block_map(debugfs, image, "/PROTECTED.TXT", 0, tool_env),
        _ext4_block_map(debugfs, image, "/PROTECTED.TXT",
                        metadata_start // filesystem_block_size, tool_env),
    )
    for suffix, physical_block in zip(("data", "tree"), corruption_blocks):
        damaged = tmp_path / f"ext4-verity-corrupt-{suffix}.img"
        shutil.copyfile(image, damaged)
        with damaged.open("r+b") as target:
            target.seek(physical_block * filesystem_block_size)
            original = target.read(1)
            assert original
            target.seek(-1, os.SEEK_CUR)
            target.write(bytes([original[0] ^ 0x01]))
        status, length, source_status = protected_payload_status(damaged)
        assert peare_lib.status_message(status) == \
            "Resource integrity verification failed"
        assert length == 0
        assert peare_lib.status_message(source_status) == \
            "Resource integrity verification failed"
    assert _sha256_file(sample) == source_hash

def test_ext4_writer_feature_profile_roundtrips_through_opener(
        writer_lib, peare_lib, tmp_path):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    env = os.environ.copy()
    env["TEMP"] = str(tmp_path)
    env["TMP"] = str(tmp_path)
    cygwin_bin = os.environ.get("PEARE_CYGWIN_BIN")
    if cygwin_bin:
        env["PATH"] = cygwin_bin + os.pathsep + env.get("PATH", "")
    image = tmp_path / "ext4-writer-feature-profile.img"
    features = ("casefold,ea_inode,mmp,verity,stable_inodes,fast_commit,"
                "orphan_file,metadata_csum,64bit,flex_bg,dir_index")
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-O", features,
         "-E", "encoding=utf8-12.1", str(image), "65536"],
        capture_output=True, text=True, timeout=60, env=env)
    assert created.returncode == 0, created.stdout + created.stderr
    for command in (
            "mkdir /CF",
            "set_inode_field /CF flags 0x40080000",
            f'write "{ROOT / "README.md"}" /CF/Alpha',
            f'write "{ROOT / "README.md"}" /CF/DeleteMe',
            f'write "{ROOT / "README.md"}" /PROTECTED.TXT'):
        result = subprocess.run(
            [debugfs, "-w", "-R", command, str(image)], capture_output=True,
            text=True, timeout=60, env=env)
        assert result.returncode == 0, result.stdout + result.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    add_source = ROOT / "README.md"
    replacement = ROOT / "src/writer/formats/ExtVolumeUpdate.cpp"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_enable_fsverity(
            writer, b"PROTECTED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"PROTECTED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"CF/Alpha") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"CF/Alpha", b"CF/Beta") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"CF/DeleteMe", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(add_source).encode(), b"CF/Created.txt") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    opener = peare_lib.open_file(str(image))
    assert opener is not None
    try:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["PROTECTED.TXT"]["sha256"] == hashlib.sha256(
            replacement.read_bytes()).hexdigest()
        assert rows["CF/Beta"]["sha256"] == hashlib.sha256(
            replacement.read_bytes()).hexdigest()
        assert rows["CF/Created.txt"]["sha256"] == hashlib.sha256(
            add_source.read_bytes()).hexdigest()
        assert "CF/DeleteMe" not in rows
    finally:
        opener.close()

@pytest.mark.parametrize("filesystem", ["ext2", "ext3"])
def test_ext2_ext3_writer_cud_roundtrips_through_opener(
        writer_lib, peare_lib, tmp_path, filesystem):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    env = os.environ.copy()
    env["TEMP"] = str(tmp_path)
    env["TMP"] = str(tmp_path)
    cygwin_bin = os.environ.get("PEARE_CYGWIN_BIN")
    if cygwin_bin:
        env["PATH"] = cygwin_bin + os.pathsep + env.get("PATH", "")
    image = tmp_path / f"{filesystem}-writer-reader.img"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", filesystem, str(image), "16384"],
        capture_output=True, text=True, timeout=60, env=env)
    assert created.returncode == 0, created.stdout + created.stderr
    seed = ROOT / "README.md"
    seeded = subprocess.run(
        [debugfs, "-w", "-R", f'write "{seed}" /SEED.TXT', str(image)],
        capture_output=True, text=True, timeout=60, env=env)
    assert seeded.returncode == 0, seeded.stdout + seeded.stderr
    baseline = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=env)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr

    replacement = ROOT / "src/writer/formats/ExtVolumeUpdate.cpp"
    created_source = ROOT / "AGENTS.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"SEED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(created_source).encode(), b"DIR/CREATED.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"DIR/CREATED.TXT", b"DIR/FINAL.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(seed).encode(), b"DIR/DELETE.TXT") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"DIR/DELETE.TXT", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    opener = peare_lib.open_file(str(image))
    assert opener is not None
    try:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["SEED.TXT"]["sha256"] == hashlib.sha256(
            replacement.read_bytes()).hexdigest()
        assert rows["DIR/FINAL.TXT"]["sha256"] == hashlib.sha256(
            created_source.read_bytes()).hexdigest()
        assert "DIR/DELETE.TXT" not in rows
    finally:
        opener.close()

@pytest.mark.parametrize("indexed", [False, True])
def test_ext4_casefold_cud_uses_case_insensitive_lookup(
        writer_lib, tmp_path, indexed):
    mke2fs = os.environ.get("PEARE_MKE2FS")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not mke2fs or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_MKE2FS, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not Path(tool).is_file() for tool in (mke2fs, debugfs, e2fsck)):
        pytest.skip("The independent e2fsprogs tools are unavailable")

    tool_env = os.environ.copy()
    cygwin_bin = os.environ.get("PEARE_CYGWIN_BIN")
    if cygwin_bin:
        tool_env["PATH"] = cygwin_bin + os.pathsep + tool_env.get("PATH", "")

    sample = tmp_path / "ext4-casefold-tool-image.img"
    features = "casefold,dir_index" if indexed else "casefold"
    created = subprocess.run(
        [mke2fs, "-F", "-q", "-t", "ext4", "-b", "4096", "-O",
         features, str(sample), "16384"],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert created.returncode == 0, created.stdout + created.stderr
    for command in ("mkdir /cf", "set_inode_field /cf flags 0x40080000"):
        configured = subprocess.run(
            [debugfs, "-w", "-R", command, str(sample)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert configured.returncode == 0, configured.stdout + configured.stderr

    source = ROOT / "README.md"
    replacement = ROOT / "THIRDPARTY.md"
    seeded = subprocess.run(
        [debugfs, "-w", "-R", f'write "{source}" /cf/Alpha', str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert seeded.returncode == 0, seeded.stdout + seeded.stderr
    if indexed:
        commands = "".join(
            f'write "{source}" /cf/Seed{index:03d}\n'
            for index in range(500))
        populated = subprocess.run(
            [debugfs, "-w", str(sample)], input=commands,
            capture_output=True, text=True, timeout=120, env=tool_env)
        assert populated.returncode == 0, populated.stdout + populated.stderr
        optimized = subprocess.run(
            [e2fsck, "-fyD", str(sample)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert optimized.returncode == 0, \
            optimized.stdout + optimized.stderr
        tree = subprocess.run(
            [debugfs, "-R", "htree_dump /cf", str(sample)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "Root node dump:" in tree.stdout, tree.stdout + tree.stderr
    stats = subprocess.run(
        [debugfs, "-R", "show_super_stats", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert "Character encoding:       utf8-12.1" in stats.stdout
    checked_sample = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked_sample.returncode == 0, \
        checked_sample.stdout + checked_sample.stderr

    original_hash = _sha256_file(sample)
    image = tmp_path / "ext4-casefold-cud.img"
    _copy_file_sha256(sample, image)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"cf/alpha") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"cf/alpha", b"cf/Beta") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"cf/Gamma") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"cf/gAMMa", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    extracted = tmp_path / "ext4-casefold-renamed-matrix.md"
    dumped = subprocess.run(
        [debugfs, "-R", f'dump -p /cf/Beta "{extracted}"', str(image)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and extracted.is_file(), \
        dumped.stdout + dumped.stderr
    assert extracted.read_bytes() == replacement.read_bytes()
    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    if indexed:
        tree = subprocess.run(
            [debugfs, "-R", "htree_dump /cf", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "Root node dump:" in tree.stdout, tree.stdout + tree.stderr
    assert _sha256_file(sample) == original_hash

    duplicate = tmp_path / "ext4-casefold-duplicate.img"
    _copy_file_sha256(sample, duplicate)
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(duplicate).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_empty_file(
            writer, b"cf/aLPHa") != 0
        writer_lib.peare_writer_abort(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(duplicate) == original_hash

    unsupported = tmp_path / "ext4-casefold-unsupported-encoding.img"
    _copy_file_sha256(sample, unsupported)
    changed = subprocess.run(
        [debugfs, "-w", "-R", "set_super_value encoding 2", str(unsupported)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert changed.returncode == 0, changed.stdout + changed.stderr
    unsupported_hash = _sha256_file(unsupported)
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(unsupported).encode(), 0) != 0
        writer_lib.peare_writer_abort(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(unsupported) == unsupported_hash

def test_ext4_xattrs_survive_cud_and_external_blocks_are_released(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_XATTR_TEST_IMAGE")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not sample_value or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_EXT_XATTR_TEST_IMAGE, PEARE_DEBUGFS, and PEARE_E2FSCK")
    sample = Path(sample_value)
    if not sample.is_file():
        pytest.skip("The independent e2fsprogs Ext4 xattr sample is not available")
    if not Path(debugfs).is_file() or not Path(e2fsck).is_file():
        pytest.skip("The independent e2fsprogs tools are not available")

    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + os.pathsep + \
        tool_env.get("PATH", "")
    image = tmp_path / "ext4-xattr-cud.img"
    original_hash = _copy_file_sha256(sample, image)
    source = ROOT / "README.md"
    checked_before = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked_before.returncode == 0, checked_before.stdout + checked_before.stderr

    external = subprocess.run(
        [debugfs, "-R", "stat /XATTR-REPLACE", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert external.returncode == 0 and "File ACL: 0" not in external.stdout, \
        external.stdout + external.stderr
    inline = subprocess.run(
        [debugfs, "-R", "ea_list /XATTR-DIR", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert inline.returncode == 0 and "user.peare.inline" in inline.stdout, \
        inline.stdout + inline.stderr
    inline_file = subprocess.run(
        [debugfs, "-R", "ea_list /XATTR-INLINE", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert inline_file.returncode == 0 and "user.peare.inline" in inline_file.stdout, \
        inline_file.stdout + inline_file.stderr

    shared_a = subprocess.run(
        [debugfs, "-R", "stat /XATTR-SHARED-A", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    shared_b = subprocess.run(
        [debugfs, "-R", "stat /XATTR-SHARED-B", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    block_a = re.search(r"File ACL: (\d+)", shared_a.stdout)
    block_b = re.search(r"File ACL: (\d+)", shared_b.stdout)
    assert block_a and block_b and block_a.group(1) == block_b.group(1), \
        shared_a.stdout + shared_b.stdout
    shared_attrs = subprocess.run(
        [debugfs, "-R", "ea_list /XATTR-SHARED-B", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert "user.peare.shared" in shared_attrs.stdout, \
        shared_attrs.stdout + shared_attrs.stderr

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"XATTR-REPLACE") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"XATTR-INLINE") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"XATTR-REPLACE", b"XATTR-RENAMED") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"XATTR-INLINE", b"XATTR-INLINE-RENAMED") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"XATTR-DIR/NEW") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
        renamed_xattr = subprocess.run(
            [debugfs, "-R", "ea_list /XATTR-RENAMED", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert renamed_xattr.returncode == 0 and \
            "user.peare.external" in renamed_xattr.stdout, \
            renamed_xattr.stdout + renamed_xattr.stderr
        preserved_inline_file = subprocess.run(
            [debugfs, "-R", "ea_list /XATTR-INLINE-RENAMED", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "user.peare.inline" in preserved_inline_file.stdout, \
            preserved_inline_file.stdout + preserved_inline_file.stderr
        preserved_inline = subprocess.run(
            [debugfs, "-R", "ea_list /XATTR-DIR", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "user.peare.inline" in preserved_inline.stdout, \
            preserved_inline.stdout + preserved_inline.stderr
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"XATTR-RENAMED", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"XATTR-INLINE-RENAMED", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"XATTR-SHARED-A", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"XATTR-DIR", 1) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
        surviving_xattr = subprocess.run(
            [debugfs, "-R", "ea_list /XATTR-SHARED-B", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "user.peare.shared" in surviving_xattr.stdout, \
            surviving_xattr.stdout + surviving_xattr.stderr
        checked_shared = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert checked_shared.returncode == 0, \
            checked_shared.stdout + checked_shared.stderr
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"XATTR-SHARED-B", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked_after = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked_after.returncode == 0, checked_after.stdout + checked_after.stderr
    for missing_path in ("/XATTR-RENAMED", "/XATTR-INLINE-RENAMED", "/XATTR-DIR",
                         "/XATTR-SHARED-A", "/XATTR-SHARED-B"):
        missing = subprocess.run(
            [debugfs, "-R", f"stat {missing_path}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "File not found" in missing.stdout + missing.stderr
    assert _sha256_file(sample) == original_hash

def test_ext4_inline_data_cud_on_external_sample(writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_INLINE_TEST_IMAGE")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not sample_value or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_EXT_INLINE_TEST_IMAGE, PEARE_DEBUGFS, and PEARE_E2FSCK")
    sample = Path(sample_value)
    if not sample.is_file() or not Path(debugfs).is_file() or \
            not Path(e2fsck).is_file():
        pytest.skip("The external e2fsprogs inline-data sample or tools are unavailable")

    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + os.pathsep + \
        tool_env.get("PATH", "")
    source = ROOT / "README.md"
    source_hash = _sha256_file(sample)
    assert source_hash.hex().upper() == \
        "DF325943409D917CB45EEE3EDDCB2540513D41B04305AE373EA925113AC54B45"
    original_check = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert original_check.returncode == 0, original_check.stdout + original_check.stderr

    for inline_path in ("/file", "/shortfile", "/link", "/dir", "/shortdir"):
        status = subprocess.run(
            [debugfs, "-R", f"stat {inline_path}", str(sample)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "flags: 0x10000000" in status.stdout.lower(), \
            status.stdout + status.stderr

    original_file = tmp_path / "inline-original-file.out"
    original_dump = subprocess.run(
        [debugfs, "-R", f"dump /file {original_file}", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert original_dump.returncode == 0, original_dump.stdout + original_dump.stderr
    original_bytes = original_file.read_bytes()
    assert original_bytes[:80] == b"x" * 80 and not any(original_bytes[80:])

    image = tmp_path / "ext4-inline-cud.img"
    _copy_file_sha256(sample, image)
    writer = _HANDLE()
    try:
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"file", b"file-renamed") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"link", b"link-renamed") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"file-renamed") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"shortfile") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"dir/created") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"dir/xxxxxxxxxxxxxxxxxxxx", b"dir/renamed") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"dir/yyyyyyyyyyyyyyyyyyyy", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"shortlink", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)

        for path in ("/file-renamed", "/shortfile"):
            extracted = tmp_path / (path[1:] + ".out")
            dump = subprocess.run(
                [debugfs, "-R", f"dump {path} {extracted}", str(image)],
                capture_output=True, text=True, timeout=60, env=tool_env)
            assert dump.returncode == 0, dump.stdout + dump.stderr
            assert extracted.read_bytes() == source.read_bytes()
        inline_attrs = subprocess.run(
            [debugfs, "-R", "ea_list /file-renamed", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "user.a" in inline_attrs.stdout, \
            inline_attrs.stdout + inline_attrs.stderr
        directory_attrs = subprocess.run(
            [debugfs, "-R", "ea_list /dir", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "user.a" in directory_attrs.stdout, \
            directory_attrs.stdout + directory_attrs.stderr
        symlink = subprocess.run(
            [debugfs, "-R", "stat /link-renamed", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "flags: 0x10000000" in symlink.stdout.lower() and \
            f'Fast link dest: "{"x" * 80}"' in symlink.stdout, \
            symlink.stdout + symlink.stderr
        listing = subprocess.run(
            [debugfs, "-R", "ls -p /dir", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "/renamed/" in listing.stdout and "/created/" in listing.stdout, \
            listing.stdout + listing.stderr
        checked = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"link-renamed", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
        removed_symlink = subprocess.run(
            [debugfs, "-R", "stat /link-renamed", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "File not found" in removed_symlink.stdout + removed_symlink.stderr
        checked = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert checked.returncode == 0, checked.stdout + checked.stderr
    finally:
        if writer:
            writer_lib.peare_writer_destroy(writer)

    recursive_image = tmp_path / "ext4-inline-recursive-delete.img"
    _copy_file_sha256(sample, recursive_image)
    recursive_writer = _HANDLE()
    try:
        assert writer_lib.peare_writer_create(C.byref(recursive_writer)) == 0
        assert writer_lib.peare_writer_update_begin_target(
            recursive_writer, str(recursive_image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(recursive_writer)
        assert writer_lib.peare_writer_delete_entry(
            recursive_writer, b"dir", 1) == 0, \
            writer_lib.peare_writer_last_error(recursive_writer)
        assert writer_lib.peare_writer_delete_entry(
            recursive_writer, b"shortdir", 1) == 0, \
            writer_lib.peare_writer_last_error(recursive_writer)
        assert writer_lib.peare_writer_update_finish(recursive_writer) == 0, \
            writer_lib.peare_writer_last_error(recursive_writer)
    finally:
        if recursive_writer:
            writer_lib.peare_writer_destroy(recursive_writer)
    recursive_check = subprocess.run(
        [e2fsck, "-fn", str(recursive_image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert recursive_check.returncode == 0, \
        recursive_check.stdout + recursive_check.stderr
    for missing_path in ("/dir", "/shortdir"):
        missing = subprocess.run(
            [debugfs, "-R", f"stat {missing_path}", str(recursive_image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert "File not found" in missing.stdout + missing.stderr
    assert _sha256_file(sample) == source_hash

def test_ext4_orphan_file_clean_profile_cud_and_reject_recovery_states(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_ORPHAN_FILE_TEST_IMAGE")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    if not sample_value or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_EXT_ORPHAN_FILE_TEST_IMAGE, PEARE_DEBUGFS, and PEARE_E2FSCK")
    sample = Path(sample_value)
    if not sample.is_file() or not Path(debugfs).is_file() or \
            not Path(e2fsck).is_file():
        pytest.skip("The external e2fsprogs orphan-file sample or tools are unavailable")

    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + os.pathsep + \
        tool_env.get("PATH", "")
    source_hash = _sha256_file(sample)
    assert source_hash.hex().upper() == \
        "46CD78DF836DEF0CD9149D18D00D5B81995B1D60EE78978272BA92877DFB79F6"
    checked_before = subprocess.run(
        [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert checked_before.returncode == 0, checked_before.stdout + checked_before.stderr

    stats = subprocess.run(
        [debugfs, "-R", "stats", str(sample)], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert "orphan_file" in stats.stdout and "Orphan file inode:" in stats.stdout, \
        stats.stdout + stats.stderr
    inode_match = re.search(r"Orphan file inode:\s*(\d+)", stats.stdout)
    assert inode_match, stats.stdout + stats.stderr
    orphan_inode = inode_match.group(1)
    orphan_dump = tmp_path / "orphan-file-before.bin"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump <{orphan_inode}> {orphan_dump}", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    assert dumped.returncode == 0 and orphan_dump.is_file(), \
        dumped.stdout + dumped.stderr
    orphan_contents = orphan_dump.read_bytes()
    assert len(orphan_contents) % 4096 == 0
    for offset in range(0, len(orphan_contents), 4096):
        block_data = orphan_contents[offset:offset + 4096]
        assert not any(block_data[:-8])
        assert block_data[-8:-4] == bytes.fromhex("04ca100b")

    image = tmp_path / "ext4-orphan-file-cud.img"
    _copy_file_sha256(sample, image)
    writer = _HANDLE()
    try:
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        source = ROOT / "README.md"
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"seed") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"created") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, b"created", b"renamed") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"renamed", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)

        extracted = tmp_path / "orphan-seed-after.bin"
        dump_after = subprocess.run(
            [debugfs, "-R", f"dump /seed {extracted}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert dump_after.returncode == 0 and \
            extracted.read_bytes() == source.read_bytes(), \
            dump_after.stdout + dump_after.stderr
        orphan_after = tmp_path / "orphan-file-after.bin"
        dump_after = subprocess.run(
            [debugfs, "-R", f"dump <{orphan_inode}> {orphan_after}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert dump_after.returncode == 0 and \
            orphan_after.read_bytes() == orphan_contents, \
            dump_after.stdout + dump_after.stderr
        checked_after = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert checked_after.returncode == 0, \
            checked_after.stdout + checked_after.stderr
    finally:
        if writer:
            writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(sample) == source_hash

    block = subprocess.run(
        [debugfs, "-R", f"bmap <{orphan_inode}> 0", str(sample)],
        capture_output=True, text=True, timeout=60, env=tool_env)
    block_match = re.search(r"(?m)^\s*(\d+)\s*$", block.stdout)
    assert block_match, block.stdout + block.stderr
    for case, debugfs_command in (
            ("orphan-present", "feature orphan_present"),
            ("bad-magic", f"zap_block -o 4088 -l 1 -p 0 {block_match.group(1)}"),
            ("bad-checksum", f"zap_block -o 4092 -l 1 -p 1 {block_match.group(1)}")):
        rejected_image = tmp_path / f"ext4-orphan-{case}.img"
        _copy_file_sha256(sample, rejected_image)
        changed = subprocess.run(
            [debugfs, "-w", "-R", debugfs_command, str(rejected_image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert changed.returncode == 0, changed.stdout + changed.stderr
        rejected_hash = _sha256_file(rejected_image)
        rejected_writer = _HANDLE()
        try:
            assert writer_lib.peare_writer_create(C.byref(rejected_writer)) == 0
            result = writer_lib.peare_writer_update_begin_target(
                rejected_writer, str(rejected_image).encode(), 0)
            assert result != 0, writer_lib.peare_writer_last_error(rejected_writer)
        finally:
            if rejected_writer:
                writer_lib.peare_writer_destroy(rejected_writer)
        assert _sha256_file(rejected_image) == rejected_hash


def test_ext4_orphan_file_block_size_and_checksum_profiles(
        writer_lib, tmp_path):
    sample_values = os.environ.get(
        "PEARE_EXT_ORPHAN_PROFILE_TEST_IMAGES", "").split(os.pathsep)
    debugfs = os.environ.get("PEARE_DEBUGFS")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    samples = [Path(value) for value in sample_values if value]
    expected = {
        "ext4-orphan-1k-csum.img":
            "E143F7C59A37A36D5F96854E10B41992684222E4568760B021E7178A48031324",
        "ext4-orphan-4k-no-csum.img":
            "9179A14CD7A78EE2076BDF43A1C467C55FEFC16AE33067DC8B2FEFC5A9C82D2A",
    }
    if not samples or not debugfs or not e2fsck:
        pytest.skip("Set PEARE_EXT_ORPHAN_PROFILE_TEST_IMAGES, PEARE_DEBUGFS, and PEARE_E2FSCK")
    if any(not sample.is_file() for sample in samples) or \
            not Path(debugfs).is_file() or not Path(e2fsck).is_file():
        pytest.skip("The external e2fsprogs orphan-file profiles or tools are unavailable")
    assert {sample.name for sample in samples} == set(expected)
    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(debugfs).parent.parent / "bin") + os.pathsep + \
        tool_env.get("PATH", "")

    for sample in samples:
        original_hash = _sha256_file(sample)
        assert original_hash.hex().upper() == expected[sample.name]
        checked = subprocess.run(
            [e2fsck, "-fn", str(sample)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        image = tmp_path / sample.name
        _copy_file_sha256(sample, image)
        writer = _HANDLE()
        try:
            assert writer_lib.peare_writer_create(C.byref(writer)) == 0
            assert writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), 0) == 0, \
                writer_lib.peare_writer_last_error(writer)
            assert writer_lib.peare_writer_create_file(
                writer, str(ROOT / "README.md").encode(), b"created") == 0, \
                writer_lib.peare_writer_last_error(writer)
            assert writer_lib.peare_writer_update_finish(writer) == 0, \
                writer_lib.peare_writer_last_error(writer)
        finally:
            if writer:
                writer_lib.peare_writer_destroy(writer)
        checked = subprocess.run(
            [e2fsck, "-fn", str(image)], capture_output=True, text=True,
            timeout=120, env=tool_env)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        assert _sha256_file(sample) == original_hash

def test_ext4_large_indexed_directory_cud_passes_e2fsck(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_EXT_LARGEDIR_IMAGE")
    e2fsck = os.environ.get("PEARE_E2FSCK")
    debugfs = os.environ.get("PEARE_DEBUGFS")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_EXT_LARGEDIR_IMAGE to the external-tool Ext4 sample")
    if not e2fsck or not Path(e2fsck).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")
    if not debugfs or not Path(debugfs).is_file():
        pytest.skip("Set PEARE_DEBUGFS to the independent debugfs executable")

    sample = Path(sample_value)
    image = tmp_path / sample.name
    source_hash = _copy_file_sha256(sample, image)
    tool_env = os.environ.copy()
    runtime_bin = Path(debugfs).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")

    checked = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=180, env=tool_env)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    tree = subprocess.run(
        [debugfs, "-R", "htree_dump /MANY", str(image)],
        capture_output=True, text=True, timeout=120, env=tool_env)
    assert tree.returncode == 0 and "Root node dump:" in tree.stdout, \
        tree.stdout + tree.stderr
    assert "Not a hash-indexed directory" not in tree.stdout
    depth = re.search(r"Indirect levels:\s*(\d+)", tree.stdout)
    assert depth and int(depth.group(1)) == 1, tree.stdout[:1000]

    target_index = _fat_writer_target_index(writer_lib, image, "Ext4")
    source = ROOT / "README.md"
    replacement = ROOT / "THIRDPARTY.md"
    replacement_payload = replacement.read_bytes()
    token = uuid.uuid4().hex[:12].upper()
    short_path = f"MANY/PEARE-{token}.TMP"
    long_root_path = f"{'R' * 243}{token}"
    created_name = ("C" * 243) + token
    final_name = ("R" * 243) + token
    empty_name = ("E" * 243) + token
    directory_name = f"PEARE-{token}"
    nested_name = f"MANY/{directory_name}/CHILD.MD"
    created_path = f"MANY/{created_name}"
    final_path = f"MANY/{final_name}"
    empty_path = f"MANY/{empty_name}"
    directory_path = f"MANY/{directory_name}"

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), target_index) == 0, \
            writer_lib.peare_writer_last_error(writer)
        for index in range(512):
            split_path = f"MANY/PEARE-{token}-{index:04X}.TMP"
            assert writer_lib.peare_writer_create_empty_file(
                writer, split_path.encode()) == 0, \
                writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_empty_file(
            writer, short_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, short_path.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_empty_file(
            writer, long_root_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, long_root_path.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), created_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), created_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, created_path.encode(), final_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_empty_file(
            writer, empty_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, empty_path.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, directory_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), nested_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, nested_name.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, directory_path.encode(), 1) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, b"MANY/FILE00001", 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    extracted = tmp_path / "large-indexed-directory-content.bin"
    dumped = subprocess.run(
        [debugfs, "-R", f"dump /{final_path} {extracted}", str(image)],
        capture_output=True, text=True, timeout=120, env=tool_env)
    assert dumped.returncode == 0 and extracted.is_file(), \
        dumped.stdout + dumped.stderr
    assert extracted.read_bytes() == replacement_payload
    assert replacement_payload
    final_check = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=180, env=tool_env)
    assert final_check.returncode == 0, \
        final_check.stdout + final_check.stderr
    final_tree = subprocess.run(
        [debugfs, "-R", "htree_dump /MANY", str(image)],
        capture_output=True, text=True, timeout=120, env=tool_env)
    final_depth = re.search(r"Indirect levels:\s*(\d+)", final_tree.stdout)
    assert final_tree.returncode == 0 and final_depth and \
        int(final_depth.group(1)) == 2, final_tree.stdout[:1000]
    for index in (0, 511):
        batch_path = f"/MANY/PEARE-{token}-{index:04X}.TMP"
        entry = subprocess.run(
            [debugfs, "-R", f"stat {batch_path}", str(image)],
            capture_output=True, text=True, timeout=60, env=tool_env)
        assert entry.returncode == 0 and "Inode:" in entry.stdout, \
            entry.stdout + entry.stderr
    assert _sha256_file(sample) == source_hash

def test_chr_ext3_cud_operations_on_disposable_copy(writer_lib, tmp_path):
    sample = os.environ.get("PEARE_EXT_DISK_TEST_IMAGE")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_DISK_TEST_IMAGE to the real CHR disk image")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")
    sample = Path(sample)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    partition_start, _ = _mbr_ext_partition(image, True)
    before = _tsk_recover_ext(
        sleuthkit_bin, image, partition_start, tmp_path / "ext3-before")
    assert before
    target_index = _fat_writer_target_index(writer_lib, image, "Ext3")
    capabilities = C.c_uint32(0)
    assert writer_lib.peare_writer_get_update_capabilities(
        str(image).encode(), target_index, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f
    cud = _fat_writer_cud(writer_lib, image, tmp_path, target_index=target_index)
    after = _tsk_recover_ext(
        sleuthkit_bin, image, partition_start, tmp_path / "ext3-after")
    assert all(after.get(path) == digest for path, digest in before.items())
    final = next(path for path in after if _fat_path_matches(path, cud["final_name"]))
    assert after[final] == hashlib.sha256(cud["replacement_payload"]).digest()
    fsstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", str(partition_start), "-f", "ext", str(image)],
        capture_output=True, text=True, timeout=60)
    assert fsstat.returncode == 0, fsstat.stdout + fsstat.stderr
    assert "File System Type: Ext3" in fsstat.stdout
    assert "Unmounted properly" in fsstat.stdout
    assert _sha256_file(sample) == original_hash

def test_chr_ext3_rejected_cud_operations_are_atomic(writer_lib, tmp_path):
    sample = os.environ.get("PEARE_EXT_DISK_TEST_IMAGE")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_DISK_TEST_IMAGE to the real CHR disk image")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")
    sample = Path(sample)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    partition_start, _ = _mbr_ext_partition(image, True)
    before = _tsk_recover_ext(
        sleuthkit_bin, image, partition_start, tmp_path / "ext3-atomic-before")
    existing_name = next(path for path in before if not path.endswith("/"))
    source = ROOT / "README.md"
    path = str(image).encode()
    rejected_operations = (
        lambda writer: writer_lib.peare_writer_create_file(
            writer, str(source).encode(), existing_name.encode()),
        lambda writer: writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"PEARE-MISSING-EXT3-FILE.BIN"),
        lambda writer: writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"../PEARE-EXT3-TRAVERSAL.BIN"),
    )
    target_index = _fat_writer_target_index(writer_lib, image, "Ext3")
    for operation in rejected_operations:
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            assert writer_lib.peare_writer_update_begin_target(
                writer, path, target_index) == 0
            assert operation(writer) != 0
            assert writer_lib.peare_writer_update_finish(writer) != 0
        finally:
            writer_lib.peare_writer_destroy(writer)
        assert _sha256_file(image) == original_hash
    after = _tsk_recover_ext(
        sleuthkit_bin, image, partition_start, tmp_path / "ext3-atomic-after")
    assert after == before
    assert _sha256_file(sample) == original_hash

@pytest.mark.parametrize(("qemu_format", "variant", "extension"), [
    ("vpc", "subformat=fixed", "vhd"),
    ("vpc", "subformat=dynamic", "vhd"),
    ("vhdx", "subformat=fixed", "vhdx"),
    ("vhdx", "subformat=dynamic", "vhdx"),
    ("vmdk", "subformat=monolithicSparse", "vmdk"),
    ("vmdk", "subformat=monolithicSparse,zeroed_grain=on", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentSparse", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentSparse,zeroed_grain=on", "vmdk"),
    ("vmdk", "subformat=monolithicFlat", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentFlat", "vmdk"),
    ("vmdk", "subformat=streamOptimized", "vmdk"),
])
def test_chr_ext2_cud_through_virtual_disk_containers(
        writer_lib, peare_lib, tmp_path, qemu_format, variant, extension):
    sample = os.environ.get("PEARE_EXT_DISK_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_DISK_TEST_IMAGE to the real CHR disk image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")

    sample = Path(sample)
    source_hash = _sha256_file(sample)
    raw_copy = tmp_path / "chr-source-copy.img"
    assert _copy_file_sha256(sample, raw_copy) == source_hash
    image = tmp_path / f"chr-ext2-{qemu_format}-{variant.replace('=', '-')}.{extension}"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", qemu_format,
         "-o", variant, str(raw_copy), str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    image_path = str(image).encode()
    assert writer_lib.peare_writer_can_add_files(image_path) == 1
    assert writer_lib.peare_writer_update_target_count(image_path) == 2
    target_name = C.create_string_buffer(256)
    assert writer_lib.peare_writer_get_add_target_name(
        image_path, 0, target_name, len(target_name)) == 0
    assert b"Ext2" in target_name.value
    original_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=0)
    }
    assert original_rows
    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    updated_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=1)
    }
    assert {path: updated_rows[path] for path in original_rows} == original_rows
    final_path = next(path for path in updated_rows
                      if _fat_path_matches(path, cud["final_name"]))
    expected_hash = hashlib.sha256(cud["replacement_payload"]).hexdigest()
    assert updated_rows[final_path] == expected_hash

    qemu_info = subprocess.run(
        [qemu_img, "info", "-f", qemu_format, "--output=json", str(image)],
        capture_output=True, text=True, timeout=60)
    assert qemu_info.returncode == 0, qemu_info.stdout + qemu_info.stderr
    assert json.loads(qemu_info.stdout)["format"] == qemu_format
    raw_result = tmp_path / f"ext2-roundtrip-{qemu_format}.img"
    roundtrip = subprocess.run(
        [qemu_img, "convert", "-f", qemu_format, "-O", "raw",
         str(image), str(raw_result)], capture_output=True, text=True, timeout=180)
    assert roundtrip.returncode == 0, roundtrip.stdout + roundtrip.stderr
    fsstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", "34", "-f", "ext", str(raw_result)],
        capture_output=True, text=True, timeout=60)
    assert fsstat.returncode == 0, fsstat.stdout + fsstat.stderr
    assert "File System Type: Ext2" in fsstat.stdout
    recovered = tmp_path / f"tsk-recovered-{qemu_format}"
    recovered.mkdir()
    extraction = subprocess.run(
        [str(Path(sleuthkit_bin) / "tsk_recover.exe"), "-i", "raw", "-b", "512",
         "-o", "34", "-f", "ext", "-e", str(raw_result), str(recovered)],
        capture_output=True, text=True, timeout=120)
    assert extraction.returncode == 0, extraction.stdout + extraction.stderr
    recovered_payload = next(recovered.rglob(cud["final_name"]))
    assert _sha256_file(recovered_payload).hex() == expected_hash
    assert _sha256_file(sample) == source_hash
    assert _sha256_file(raw_copy) == source_hash

@pytest.mark.parametrize(("qemu_format", "variant", "extension"), [
    ("vpc", "subformat=fixed", "vhd"),
    ("vpc", "subformat=dynamic", "vhd"),
    ("vhdx", "subformat=fixed", "vhdx"),
    ("vhdx", "subformat=dynamic", "vhdx"),
    ("vmdk", "subformat=monolithicSparse", "vmdk"),
    ("vmdk", "subformat=monolithicSparse,zeroed_grain=on", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentSparse", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentSparse,zeroed_grain=on", "vmdk"),
    ("vmdk", "subformat=monolithicFlat", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentFlat", "vmdk"),
    ("vmdk", "subformat=streamOptimized", "vmdk"),
])
def test_chr_ext3_cud_through_virtual_disk_containers(
        writer_lib, tmp_path, qemu_format, variant, extension):
    sample = os.environ.get("PEARE_EXT_DISK_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_DISK_TEST_IMAGE to the real CHR disk image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")

    sample = Path(sample)
    source_hash = _sha256_file(sample)
    partition_start, _ = _mbr_ext_partition(sample, True)
    original_rows = _tsk_recover_ext(
        sleuthkit_bin, sample, partition_start, tmp_path / "ext3-source")
    raw_copy = tmp_path / "chr-ext3-source-copy.img"
    assert _copy_file_sha256(sample, raw_copy) == source_hash
    image = tmp_path / f"chr-ext3-{qemu_format}-{variant.replace('=', '-')}.{extension}"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", qemu_format,
         "-o", variant, str(raw_copy), str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    target_index = _fat_writer_target_index(writer_lib, image, "Ext3")
    duplicate_name = next(iter(original_rows))
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, target_index, duplicate_name)
    cud = _fat_writer_cud(writer_lib, image, tmp_path, target_index=target_index)
    qemu_info = subprocess.run(
        [qemu_img, "info", "-f", qemu_format, "--output=json", str(image)],
        capture_output=True, text=True, timeout=60)
    assert qemu_info.returncode == 0, qemu_info.stdout + qemu_info.stderr
    assert json.loads(qemu_info.stdout)["format"] == qemu_format
    raw_result = tmp_path / f"ext3-roundtrip-{qemu_format}-{variant.replace('=', '-')}.img"
    roundtrip = subprocess.run(
        [qemu_img, "convert", "-f", qemu_format, "-O", "raw",
         str(image), str(raw_result)], capture_output=True, text=True, timeout=180)
    assert roundtrip.returncode == 0, roundtrip.stdout + roundtrip.stderr
    after = _tsk_recover_ext(
        sleuthkit_bin, raw_result, partition_start, tmp_path / "ext3-after")
    assert all(after.get(path) == digest for path, digest in original_rows.items())
    final = next(path for path in after if _fat_path_matches(path, cud["final_name"]))
    assert after[final] == hashlib.sha256(cud["replacement_payload"]).digest()
    fsstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", str(partition_start), "-f", "ext", str(raw_result)],
        capture_output=True, text=True, timeout=60)
    assert fsstat.returncode == 0, fsstat.stdout + fsstat.stderr
    assert "File System Type: Ext3" in fsstat.stdout
    assert "Unmounted properly" in fsstat.stdout
    assert _sha256_file(sample) == source_hash
    assert _sha256_file(raw_copy) == source_hash

def test_chr_ext2_rejected_cud_operations_are_atomic(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_EXT_DISK_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_DISK_TEST_IMAGE to the real CHR disk image")
    sample = Path(sample)
    image = tmp_path / sample.name
    _copy_file_sha256(sample, image)
    rows = _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=0)
    existing_file = next(row["path"] for row in rows if not row["is_container"])
    existing_name = existing_file.split("/", 1)[-1].encode()
    image_path = str(image).encode()
    source = ROOT / "README.md"
    rejected_operations = (
        lambda writer: writer_lib.peare_writer_create_file(
            writer, str(source).encode(), existing_name),
        lambda writer: writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"PEARE-MISSING-EXT2-FILE.BIN"),
        lambda writer: writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"../PEARE-EXT2-TRAVERSAL.BIN"),
    )
    original_hash = _sha256_file(image)
    original_rows = {row["path"]: row["sha256"] for row in rows}
    for operation in rejected_operations:
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            assert writer_lib.peare_writer_update_begin_target(
                writer, image_path, 0) == 0
            assert operation(writer) != 0
            assert writer_lib.peare_writer_update_finish(writer) != 0
        finally:
            writer_lib.peare_writer_destroy(writer)
        assert _sha256_file(image) == original_hash
        assert {
            row["path"]: row["sha256"]
            for row in _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=0)
        } == original_rows

def test_tar_cud_with_archive_created_by_external_tool(
        writer_lib, peare_lib, tmp_path):
    tar_tool = shutil.which("tar.exe") or shutil.which("tar")
    if not tar_tool:
        pytest.skip("A system tar tool is required to create the sample archive")
    archive = tmp_path / "external-sample.tar"
    original = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    created_source = ROOT / "README.md"
    replacement_source = ROOT / "AGENTS.md"
    creation = subprocess.run(
        [tar_tool, "-cf", str(archive), "-C", str(original.parent), original.name],
        capture_output=True, text=True, timeout=30)
    assert creation.returncode == 0, creation.stdout + creation.stderr
    assert writer_lib.peare_writer_can_add_files(str(archive).encode()) == 1
    capabilities = C.c_uint32()
    assert writer_lib.peare_writer_get_update_capabilities(
        str(archive).encode(), 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f

    _assert_archive_duplicate_create_is_atomic(
        writer_lib, archive, original.name, tmp_path)
    assert writer_lib.peare_writer_update_target_count(str(archive).encode()) == 1
    capabilities = C.c_uint32()
    assert writer_lib.peare_writer_get_update_capabilities(
        str(archive).encode(), 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(created_source).encode(), b"CUD-CREATED.TXT") == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement_source).encode(), b"CUD-CREATED.TXT") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"CUD-CREATED.TXT", b"CUD-FINAL.TXT") == 0
        assert writer_lib.peare_writer_create_empty_file(writer, b"CUD-EMPTY.TXT") == 0
        assert writer_lib.peare_writer_create_directory(writer, b"CUD-DIR") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(created_source).encode(), b"CUD-DIR/CHILD.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"CUD-EMPTY.TXT", 0) == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"CUD-DIR", 1) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)

    with tarfile.open(archive, "r:") as updated:
        members = {member.name: member for member in updated.getmembers()}
        assert set(members) == {original.name, "CUD-FINAL.TXT"}
        assert updated.extractfile(members[original.name]).read() == original.read_bytes()
        assert updated.extractfile(members["CUD-FINAL.TXT"]).read() == replacement_source.read_bytes()
    opener = peare_lib.open_file(str(archive))
    assert opener is not None
    try:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows[original.name]["sha256"] == hashlib.sha256(
            original.read_bytes()).hexdigest()
        assert rows["CUD-FINAL.TXT"]["sha256"] == hashlib.sha256(
            replacement_source.read_bytes()).hexdigest()
        assert "CUD-EMPTY.TXT" not in rows
        assert "CUD-DIR/CHILD.TXT" not in rows
    finally:
        opener.close()

def test_tar_gzip_create_and_cud_roundtrip_with_7zip(
        writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required for independent TAR.GZ validation")

    payload = tmp_path / "payload.txt"
    payload.write_bytes((ROOT / "README.md").read_bytes())
    ordinary_gzip = tmp_path / "ordinary.gz"
    compressed = subprocess.run(
        [seven_zip, "a", "-tgzip", str(ordinary_gzip), str(payload)],
        capture_output=True, text=True, timeout=60)
    assert compressed.returncode == 0, compressed.stdout + compressed.stderr
    assert writer_lib.peare_writer_can_add_files(str(ordinary_gzip).encode()) == 0

    archive = tmp_path / "created.tgz"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"tar-gzip", b"ustar", str(archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(writer, b"docs") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(payload).encode(), b"docs/payload.txt") == 0
        assert writer_lib.peare_writer_create_empty_file(
            writer, b"empty.txt") == 0
        assert writer_lib.peare_writer_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with tarfile.open(archive, "r:gz") as created:
        assert set(created.getnames()) == {"docs", "docs/payload.txt", "empty.txt"}
        assert created.extractfile("docs/payload.txt").read() == payload.read_bytes()
        assert created.extractfile("empty.txt").read() == b""
    checked = subprocess.run(
        [seven_zip, "t", str(archive)], capture_output=True, text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    opened = peare_lib.open_file(str(archive))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert rows["archive.tar/docs/payload.txt"] == hashlib.sha256(
            payload.read_bytes()).hexdigest()
    finally:
        opened.close()

    batch_archive = tmp_path / "batch.tgz"
    batch_input = _WriterInputFile(str(payload).encode(), b"batch.txt")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_to(
            writer, str(batch_archive).encode(), b"tar-gzip", b"ustar", 0,
            C.byref(batch_input), 1) == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with tarfile.open(batch_archive, "r:gz") as batch:
        assert batch.getnames() == ["batch.txt"]
        assert batch.extractfile("batch.txt").read() == payload.read_bytes()
    checked = subprocess.run(
        [seven_zip, "t", str(batch_archive)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr

    sample = ROOT / "tests" / "formats" / "tar-compressed" / "fixtures" / "docs.tgz"
    legacy_archive = tmp_path / "legacy-add.tgz"
    shutil.copyfile(sample, legacy_archive)
    legacy_payload = tmp_path / "legacy-payload.txt"
    legacy_payload.write_bytes(b"legacy TAR.GZ add path")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_begin(
            writer, str(legacy_archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_add_file(
            writer, str(legacy_payload).encode(), b"legacy-added.txt") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with tarfile.open(legacy_archive, "r:gz") as legacy:
        assert legacy.extractfile("legacy-added.txt").read() == legacy_payload.read_bytes()
    checked = subprocess.run(
        [seven_zip, "t", str(legacy_archive)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr

    updated_archive = tmp_path / "updated.tgz"
    sample_hash = _sha256_file(sample)
    shutil.copyfile(sample, updated_archive)
    assert writer_lib.peare_writer_can_add_files(str(updated_archive).encode()) == 1
    with tarfile.open(sample, "r:gz") as original:
        original_rows = {member.name for member in original.getmembers()}
    replacement = ROOT / "AGENTS.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(updated_archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"README.md") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"README.md", b"RENAMED.md") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(payload).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"docs/TESTING.md", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with tarfile.open(updated_archive, "r:gz") as updated:
        rows = {member.name: member for member in updated.getmembers()}
        assert set(rows) == (original_rows - {"README.md", "docs/TESTING.md"}) | {
            "RENAMED.md", "added.txt"}
        assert updated.extractfile(rows["RENAMED.md"]).read() == replacement.read_bytes()
        assert updated.extractfile(rows["added.txt"]).read() == payload.read_bytes()
    checked = subprocess.run(
        [seven_zip, "t", str(updated_archive)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == sample_hash
    opened = peare_lib.open_file(str(updated_archive))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert rows["archive.tar/RENAMED.md"] == hashlib.sha256(
            replacement.read_bytes()).hexdigest()
        assert rows["archive.tar/added.txt"] == hashlib.sha256(
            payload.read_bytes()).hexdigest()
        assert "archive.tar/docs/TESTING.md" not in rows
    finally:
        opened.close()

def test_tar_bzip2_create_and_cud_roundtrip_with_7zip(
        writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required for independent TAR.BZ2 validation")

    payload = tmp_path / "payload.txt"
    payload.write_bytes((ROOT / "README.md").read_bytes())
    assert writer_lib.peare_writer_create_format_count() == 30
    assert writer_lib.peare_writer_create_variant_count(b"tar-bzip2") == 9
    for level in range(1, 10):
        archive = tmp_path / f"created-level-{level}.tbz2"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            variant = f"level-{level}".encode()
            assert writer_lib.peare_writer_create_begin_variant(
                writer, b"tar-bzip2", variant, str(archive).encode()) == 0, \
                writer_lib.peare_writer_last_error(writer)
            assert writer_lib.peare_writer_create_file(
                writer, str(payload).encode(), b"payload.txt") == 0
            assert writer_lib.peare_writer_finish(writer) == 0, \
                writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        with tarfile.open(archive, "r:bz2") as created:
            assert created.getnames() == ["payload.txt"]
            assert created.extractfile("payload.txt").read() == payload.read_bytes()
        checked = subprocess.run(
            [seven_zip, "t", str(archive)], capture_output=True,
            text=True, timeout=60)
        assert checked.returncode == 0, checked.stdout + checked.stderr

    batch_archive = tmp_path / "batch.tbz2"
    batch_input = _WriterInputFile(str(payload).encode(), b"batch.txt")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_to(
            writer, str(batch_archive).encode(), b"tar-bzip2", b"level-9", 0,
            C.byref(batch_input), 1) == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with tarfile.open(batch_archive, "r:bz2") as batch:
        assert batch.getnames() == ["batch.txt"]
        assert batch.extractfile("batch.txt").read() == payload.read_bytes()
    checked = subprocess.run(
        [seven_zip, "t", str(batch_archive)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr

    ordinary_bzip2 = tmp_path / "ordinary.bz2"
    compressed = subprocess.run(
        [seven_zip, "a", "-tbzip2", str(ordinary_bzip2), str(payload)],
        capture_output=True, text=True, timeout=60)
    assert compressed.returncode == 0, compressed.stdout + compressed.stderr
    assert writer_lib.peare_writer_can_add_files(str(ordinary_bzip2).encode()) == 0

    sample = ROOT / "tests" / "formats" / "tar-compressed" / "fixtures" / "docs.tbz2"
    updated_archive = tmp_path / "updated.tbz2"
    sample_hash = _sha256_file(sample)
    shutil.copyfile(sample, updated_archive)
    assert writer_lib.peare_writer_can_add_files(str(updated_archive).encode()) == 1
    replacement = ROOT / "AGENTS.md"
    with tarfile.open(sample, "r:bz2") as original:
        original_rows = {member.name for member in original.getmembers()}
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(updated_archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"README.md") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"README.md", b"RENAMED.md") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(payload).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"docs/TESTING.md", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with tarfile.open(updated_archive, "r:bz2") as updated:
        rows = {member.name: member for member in updated.getmembers()}
        assert set(rows) == (original_rows - {"README.md", "docs/TESTING.md"}) | {
            "RENAMED.md", "added.txt"}
        assert updated.extractfile(rows["RENAMED.md"]).read() == replacement.read_bytes()
        assert updated.extractfile(rows["added.txt"]).read() == payload.read_bytes()
    checked = subprocess.run(
        [seven_zip, "t", str(updated_archive)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == sample_hash
    opened = peare_lib.open_file(str(updated_archive))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert rows["updated.tbz2/RENAMED.md"] == hashlib.sha256(
            replacement.read_bytes()).hexdigest()
        assert rows["updated.tbz2/added.txt"] == hashlib.sha256(
            payload.read_bytes()).hexdigest()
        assert "updated.tbz2/docs/TESTING.md" not in rows
    finally:
        opened.close()

    tbz_sample = ROOT / "tests" / "formats" / "tar-compressed" / "fixtures" / "docs.tbz"
    tbz_archive = tmp_path / "updated.tbz"
    tbz_hash = _sha256_file(tbz_sample)
    shutil.copyfile(tbz_sample, tbz_archive)
    assert writer_lib.peare_writer_can_add_files(str(tbz_archive).encode()) == 1
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(tbz_archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(payload).encode(), b"alias-added.txt") == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with tarfile.open(tbz_archive, "r:bz2") as updated:
        assert updated.extractfile("alias-added.txt").read() == payload.read_bytes()
    checked = subprocess.run(
        [seven_zip, "t", str(tbz_archive)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(tbz_sample) == tbz_hash

def test_tar_zstd_create_and_cud_roundtrip_with_independent_decoder(
        writer_lib, peare_lib, tmp_path):
    def read_tar(path):
        with path.open("rb") as compressed:
            with zstandard.ZstdDecompressor().stream_reader(compressed) as reader:
                decoded = reader.read()
        with tarfile.open(fileobj=io.BytesIO(decoded), mode="r:") as archive:
            return {member.name: archive.extractfile(member).read()
                    for member in archive.getmembers() if member.isfile()}

    payload = tmp_path / "payload.txt"
    payload.write_bytes((ROOT / "README.md").read_bytes())
    assert writer_lib.peare_writer_create_format_count() == 30
    assert writer_lib.peare_writer_create_variant_count(b"tar-zstd") == 27
    variants = [f"fast-{level}" for level in range(5, 0, -1)] + [
        f"level-{level}" for level in range(1, 23)]
    for variant in variants:
        archive = tmp_path / f"created-{variant}.tar.zst"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            assert writer_lib.peare_writer_create_begin_variant(
                writer, b"tar-zstd", variant.encode(), str(archive).encode()) == 0, \
                writer_lib.peare_writer_last_error(writer)
            assert writer_lib.peare_writer_create_file(
                writer, str(payload).encode(), b"payload.txt") == 0
            assert writer_lib.peare_writer_finish(writer) == 0, \
                writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        created = read_tar(archive)
        assert list(created) == ["payload.txt"]
        assert created["payload.txt"] == payload.read_bytes()

    batch_archive = tmp_path / "batch.tar.zst"
    batch_input = _WriterInputFile(str(payload).encode(), b"batch.txt")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_to(
            writer, str(batch_archive).encode(), b"tar-zstd", b"level-3", 0,
            C.byref(batch_input), 1) == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    batch = read_tar(batch_archive)
    assert list(batch) == ["batch.txt"]
    assert batch["batch.txt"] == payload.read_bytes()

    plain_zstd = ROOT / "tests" / "formats" / "zstd" / "fixtures" / "readme.zst"
    assert writer_lib.peare_writer_can_add_files(str(plain_zstd).encode()) == 0
    sample = ROOT / "tests" / "formats" / "zstd" / "fixtures" / "docs.tar.zst"
    updated_archive = tmp_path / "updated.tar.zst"
    sample_hash = _sha256_file(sample)
    shutil.copyfile(sample, updated_archive)
    assert writer_lib.peare_writer_can_add_files(str(updated_archive).encode()) == 1
    replacement = ROOT / "AGENTS.md"
    original_rows = set(read_tar(sample))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(updated_archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"README.md") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"README.md", b"RENAMED.md") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(payload).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"docs/TESTING.md", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    rows = read_tar(updated_archive)
    assert set(rows) == (original_rows - {"README.md", "docs/TESTING.md"}) | {
        "RENAMED.md", "added.txt"}
    assert rows["RENAMED.md"] == replacement.read_bytes()
    assert rows["added.txt"] == payload.read_bytes()
    assert _sha256_file(sample) == sample_hash
    opened = peare_lib.open_file(str(updated_archive))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert rows["updated.tar/RENAMED.md"] == hashlib.sha256(
            replacement.read_bytes()).hexdigest()
        assert rows["updated.tar/added.txt"] == hashlib.sha256(
            payload.read_bytes()).hexdigest()
        assert "updated.tar/docs/TESTING.md" not in rows
    finally:
        opened.close()

    for suffix in (".tzst", ".tar.zstd"):
        alias_archive = tmp_path / f"updated{suffix}"
        shutil.copyfile(sample, alias_archive)
        assert writer_lib.peare_writer_can_add_files(
            str(alias_archive).encode()) == 1
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            assert writer_lib.peare_writer_update_begin(
                writer, str(alias_archive).encode()) == 0, \
                writer_lib.peare_writer_last_error(writer)
            assert writer_lib.peare_writer_create_file(
                writer, str(payload).encode(), b"alias-added.txt") == 0
            assert writer_lib.peare_writer_update_finish(writer) == 0, \
                writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        assert read_tar(alias_archive)["alias-added.txt"] == payload.read_bytes()


def test_tar_unix_compress_create_all_variants_and_7zip(
        writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required for independent TAR.Z validation")

    source = ROOT / "src" / "writer" / "third_party" / "zstd" / "compress" / "zstd_compress.c"
    expected_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    variants = [f"block-{width}" for width in range(9, 17)] + [
        f"legacy-{width}" for width in range(9, 17)]
    assert writer_lib.peare_writer_create_variant_count(b"tar-unix-compress") == 16
    reported = []
    for index in range(16):
        item = _WriterVariant()
        assert writer_lib.peare_writer_get_create_variant(
            b"tar-unix-compress", index, C.byref(item)) == 0
        reported.append(item.id.decode())
    assert reported == variants

    input_file = _WriterInputFile(str(source).encode(), source.name.encode())
    for variant in variants:
        archive = tmp_path / f"created-{variant}.taz"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            status = writer_lib.peare_writer_add_files_to(
                writer, str(archive).encode(), b"tar-unix-compress",
                variant.encode(), 0, C.byref(input_file), 1)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)

        if not (variant.endswith("-9") and
                _seven_zip_version(seven_zip) == (26, 5)):
            checked = subprocess.run(
                [seven_zip, "t", str(archive)], capture_output=True,
                text=True, timeout=60)
            assert checked.returncode == 0, checked.stdout + checked.stderr
        with peare_lib.open_file(str(archive)) as opener:
            rows = opener.normalized_tree()
            assert any(row["path"].endswith("/" + source.name) and
                       row["sha256"] == expected_hash for row in rows)


def test_tar_unix_compress_cud_from_upstream_sample(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    root = Path(os.environ.get(
        "PEARE_COMPRESS_SAMPLE_ROOT",
        Path.home() / "Documents" / "inerente a" / "Peare" /
        "compress-samples" / "libarchive"))
    sample = root / "test_compat_mac-1.tar.Z"
    if not sample.is_file():
        pytest.skip("The upstream libarchive TAR.Z sample is not configured")
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required for independent TAR.Z validation")

    payload = ROOT / "README.md"
    replacement = ROOT / "AGENTS.md"
    updated = tmp_path / "updated.tar.Z"
    original_hash = _sha256_file(sample)
    shutil.copyfile(sample, updated)
    assert writer_lib.peare_writer_can_add_files(str(updated).encode()) == 1
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(updated).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"file") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"file", b"renamed.txt") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(payload).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"._file", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    checked = subprocess.run(
        [seven_zip, "t", str(updated)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash
    with peare_lib.open_file(str(updated)) as opener:
        rows = {row["path"]: row["sha256"] for row in opener.normalized_tree()}
        root_name = "updated.tar"
        assert rows[f"{root_name}/renamed.txt"] == hashlib.sha256(
            replacement.read_bytes()).hexdigest()
        assert rows[f"{root_name}/added.txt"] == hashlib.sha256(
            payload.read_bytes()).hexdigest()
        assert f"{root_name}/file" not in rows
        assert f"{root_name}/._file" not in rows

    for suffix in (".taz", ".tar.Z"):
        alias = tmp_path / f"alias{suffix}"
        shutil.copyfile(sample, alias)
        assert writer_lib.peare_writer_can_add_files(str(alias).encode()) == 1

    for view in ("tree", "details"):
        scenario = {
            "view": view,
            "target_index": 0,
            "operations": [
                {"type": "create_directory", "name": "WriterTest"},
                {"type": "add_files", "view_path": "writer-test-copy",
                 "destination": "WriterTest", "sources": [str(payload)],
                 "expect_present": "writer-test-copy/WriterTest/README.md"},
                {"type": "rename", "path": "writer-test-copy/WriterTest/README.md",
                 "view_path": "writer-test-copy/WriterTest", "name": "renamed.txt",
                 "expect_present": "writer-test-copy/WriterTest/renamed.txt"},
                {"type": "delete", "path": "writer-test-copy/WriterTest/renamed.txt",
                 "view_path": "writer-test-copy/WriterTest",
                 "expect_absent": "writer-test-copy/WriterTest/renamed.txt"},
            ],
        }
        scenario_path = tmp_path / f"tar-z-gui-{view}.json"
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        output_dir = tmp_path / f"tar-z-gui-{view}"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
             "--exe", str(peare_gui_path), "--image", str(sample),
             "--scenario", str(scenario_path), "--output-dir", str(output_dir)],
            capture_output=True, text=True, timeout=240)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads((output_dir / "writer-test-report.json").read_text(
            encoding="utf-8"))
        assert all(operation["passed"] for operation in report["operations"])

def test_tar_xz_create_all_checks_and_7zip(writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required for independent TAR.XZ validation")
    source = ROOT / "README.md"
    payload_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    checks = ("none", "crc32", "crc64", "sha256")
    assert writer_lib.peare_writer_create_format_count() == 30
    assert writer_lib.peare_writer_create_variant_count(b"tar-xz") == len(checks)
    item = _WriterInputFile(str(source).encode(), source.name.encode())
    for check in checks:
        archive = tmp_path / f"created-{check}.txz"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            assert writer_lib.peare_writer_add_files_to(
                writer, str(archive).encode(), b"tar-xz", check.encode(),
                0, C.byref(item), 1) == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        checked = subprocess.run(
            [seven_zip, "t", str(archive)], capture_output=True,
            text=True, timeout=60)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        with lzma.open(archive, "rb") as compressed:
            with tarfile.open(fileobj=io.BytesIO(compressed.read()), mode="r:") as tar:
                member = tar.extractfile("README.md")
                assert member is not None
                assert hashlib.sha256(member.read()).hexdigest() == payload_hash
        with peare_lib.open_file(str(archive)) as opener:
            rows = opener.normalized_tree()
            assert any(row["path"].endswith("/README.md") and
                       row["sha256"] == payload_hash for row in rows)

def test_tar_xz_cud_from_independent_sample(writer_lib, peare_lib,
                                             peare_gui_path, tmp_path):
    sample = ROOT / "tests" / "formats" / "xz" / "fixtures" / "docs.tar.xz"
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required for independent TAR.XZ validation")
    updated = tmp_path / "updated.txz"
    original_hash = _sha256_file(sample)
    assert writer_lib.peare_writer_can_add_files(str(sample).encode()) == 1
    shutil.copyfile(sample, updated)
    can_add = writer_lib.peare_writer_can_add_files(str(updated).encode())
    if not can_add:
        diagnostic = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(diagnostic)) == 0
        try:
            begin_status = writer_lib.peare_writer_update_begin(
                diagnostic, str(updated).encode())
            error = writer_lib.peare_writer_last_error(diagnostic)
            pytest.fail(
                f"TAR.XZ update probe rejected the sample: status={begin_status}, "
                f"error={error.decode(errors='replace') if error else ''}")
        finally:
            writer_lib.peare_writer_destroy(diagnostic)
    replacement = ROOT / "AGENTS.md"
    added = ROOT / "THIRDPARTY.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(updated).encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"README.md") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"docs/TESTING.md", b"docs/TESTING-renamed.md") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(added).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"tests/POLICY.md", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    checked = subprocess.run(
        [seven_zip, "t", str(updated)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash
    with lzma.open(updated, "rb") as compressed:
        with tarfile.open(fileobj=io.BytesIO(compressed.read()), mode="r:") as tar:
            rows = {member.name: tar.extractfile(member).read()
                    for member in tar.getmembers() if member.isfile()}
    assert rows["README.md"] == replacement.read_bytes()
    assert rows["added.txt"] == added.read_bytes()
    assert "docs/TESTING-renamed.md" in rows
    assert "docs/TESTING.md" not in rows
    assert "tests/POLICY.md" not in rows
    with peare_lib.open_file(str(updated)) as opener:
        paths = {row["path"]: row["sha256"] for row in opener.normalized_tree()}
        replacement_hash = hashlib.sha256(replacement.read_bytes()).hexdigest()
        assert any(path.endswith("/README.md") and value == replacement_hash
                   for path, value in paths.items())
        assert any(path.endswith("/docs/TESTING-renamed.md") for path in paths)
        assert not any(path.endswith("/tests/POLICY.md") for path in paths)

    gui_tar_root = "writer-test-copy"
    for view in ("tree", "details"):
        scenario = {
            "view": view,
            "target_index": 0,
            "operations": [
                {"type": "create_directory", "view_path": gui_tar_root,
                 "name": "WriterTest"},
                {"type": "add_files", "view_path": gui_tar_root,
                 "destination": "WriterTest", "sources": [str(added)],
                 "expect_present": f"{gui_tar_root}/WriterTest/THIRDPARTY.md"},
                {"type": "rename",
                 "path": f"{gui_tar_root}/WriterTest/THIRDPARTY.md",
                 "view_path": f"{gui_tar_root}/WriterTest", "name": "renamed.txt",
                 "expect_present": f"{gui_tar_root}/WriterTest/renamed.txt"},
                {"type": "delete",
                 "path": f"{gui_tar_root}/WriterTest/renamed.txt",
                 "view_path": f"{gui_tar_root}/WriterTest",
                 "expect_absent": f"{gui_tar_root}/WriterTest/renamed.txt"},
            ],
        }
        scenario_path = tmp_path / f"tar-xz-gui-{view}.json"
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        output_dir = tmp_path / f"tar-xz-gui-{view}"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
             "--exe", str(peare_gui_path), "--image", str(sample),
             "--scenario", str(scenario_path), "--output-dir", str(output_dir)],
            capture_output=True, text=True, timeout=240)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads((output_dir / "writer-test-report.json").read_text(
            encoding="utf-8"))
        assert all(operation["passed"] for operation in report["operations"])

def test_tar_lzma_create_all_levels_and_7zip(writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required for independent TAR.LZMA validation")
    source = ROOT / "README.md"
    payload_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    assert writer_lib.peare_writer_create_format_count() == 30
    assert writer_lib.peare_writer_create_variant_count(b"tar-lzma") == 9
    item = _WriterInputFile(str(source).encode(), source.name.encode())
    for level in range(1, 10):
        archive = tmp_path / f"created-level-{level}.tar.lzma"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            assert writer_lib.peare_writer_add_files_to(
                writer, str(archive).encode(), b"tar-lzma",
                f"level-{level}".encode(), 0, C.byref(item), 1) == 0, \
                writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        checked = subprocess.run(
            [seven_zip, "t", str(archive)], capture_output=True,
            text=True, timeout=60)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        with lzma.open(archive, "rb") as compressed:
            with tarfile.open(fileobj=io.BytesIO(compressed.read()), mode="r:") as tar:
                member = tar.extractfile("README.md")
                assert member is not None
                assert hashlib.sha256(member.read()).hexdigest() == payload_hash
        with peare_lib.open_file(str(archive)) as opener:
            rows = opener.normalized_tree()
            assert any(row["path"].endswith("/README.md") and
                       row["sha256"] == payload_hash for row in rows)

def test_tar_lzma_cud_from_independent_sample(writer_lib, peare_lib,
                                               peare_gui_path, tmp_path):
    sample = ROOT / "tests" / "formats" / "lzma" / "fixtures" / "docs.tar.lzma"
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required for independent TAR.LZMA validation")
    updated = tmp_path / "updated.tar.lzma"
    original_hash = _sha256_file(sample)
    shutil.copyfile(sample, updated)
    assert writer_lib.peare_writer_can_add_files(str(updated).encode()) == 1
    replacement = ROOT / "AGENTS.md"
    added = ROOT / "THIRDPARTY.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(updated).encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"README.md") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"docs/TESTING.md", b"docs/TESTING-renamed.md") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(added).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"tests/POLICY.md", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    checked = subprocess.run(
        [seven_zip, "t", str(updated)], capture_output=True,
        text=True, timeout=60)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert _sha256_file(sample) == original_hash
    with lzma.open(updated, "rb") as compressed:
        with tarfile.open(fileobj=io.BytesIO(compressed.read()), mode="r:") as tar:
            rows = {member.name: tar.extractfile(member).read()
                    for member in tar.getmembers() if member.isfile()}
    assert rows["README.md"] == replacement.read_bytes()
    assert rows["added.txt"] == added.read_bytes()
    assert "docs/TESTING-renamed.md" in rows
    assert "docs/TESTING.md" not in rows
    assert "tests/POLICY.md" not in rows
    with peare_lib.open_file(str(updated)) as opener:
        paths = {row["path"]: row["sha256"] for row in opener.normalized_tree()}
        replacement_hash = hashlib.sha256(replacement.read_bytes()).hexdigest()
        assert any(path.endswith("/README.md") and value == replacement_hash
                   for path, value in paths.items())
        assert any(path.endswith("/docs/TESTING-renamed.md") for path in paths)
        assert not any(path.endswith("/tests/POLICY.md") for path in paths)

    for view in ("tree", "details"):
        scenario = {
            "view": view,
            "target_index": 0,
            "operations": [
                {"type": "create_directory", "view_path": "writer-test-copy",
                 "name": "WriterTest"},
                {"type": "add_files", "view_path": "writer-test-copy",
                 "destination": "WriterTest", "sources": [str(added)],
                 "expect_present": "writer-test-copy/WriterTest/THIRDPARTY.md"},
                {"type": "rename",
                 "path": "writer-test-copy/WriterTest/THIRDPARTY.md",
                 "view_path": "writer-test-copy/WriterTest", "name": "renamed.txt",
                 "expect_present": "writer-test-copy/WriterTest/renamed.txt"},
                {"type": "delete",
                 "path": "writer-test-copy/WriterTest/renamed.txt",
                 "view_path": "writer-test-copy/WriterTest",
                 "expect_absent": "writer-test-copy/WriterTest/renamed.txt"},
            ],
        }
        scenario_path = tmp_path / f"tar-lzma-gui-{view}.json"
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        output_dir = tmp_path / f"tar-lzma-gui-{view}"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
             "--exe", str(peare_gui_path), "--image", str(sample),
             "--scenario", str(scenario_path), "--output-dir", str(output_dir)],
            capture_output=True, text=True, timeout=240)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads((output_dir / "writer-test-report.json").read_text(
            encoding="utf-8"))
        assert all(operation["passed"] for operation in report["operations"])

def test_lzip_writer_all_levels_and_winrar(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    source = ROOT / "README.md"
    expected = source.read_bytes()
    expected_hash = hashlib.sha256(expected).hexdigest()
    assert writer_lib.peare_writer_create_format_count() == 30
    assert writer_lib.peare_writer_create_variant_count(b"lzip") == 10
    variants = []
    for index in range(10):
        item = _WriterVariant()
        assert writer_lib.peare_writer_get_create_variant(
            b"lzip", index, C.byref(item)) == 0
        variants.append(item.id.decode())
    assert variants == [f"level-{level}" for level in range(10)]

    source_item = _WriterInputFile(str(source).encode(), source.name.encode())
    winrar = _winrar_executable()
    gui_archive = None
    for level, variant in enumerate(variants):
        archive = tmp_path / f"created-level-{level}.lz"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            status = writer_lib.peare_writer_add_files_to(
                writer, str(archive).encode(), b"lzip", variant.encode(),
                0, C.byref(source_item), 1)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        if level == 6:
            gui_archive = archive

        encoded = archive.read_bytes()
        assert encoded[:4] == b"LZIP" and encoded[4] == 1
        exponent = encoded[5] & 0x1f
        fraction = encoded[5] >> 5
        dictionary = (1 << exponent) - ((1 << exponent) >> 4) * fraction
        decoded = lzma.decompress(
            encoded[6:-20], format=lzma.FORMAT_RAW,
            filters=[{"id": lzma.FILTER_LZMA1, "dict_size": dictionary,
                      "lc": 3, "lp": 0, "pb": 2}])
        checksum, data_size, member_size = struct.unpack("<IQQ", encoded[-20:])
        assert decoded == expected
        assert checksum == zlib.crc32(expected)
        assert data_size == len(expected)
        assert member_size == len(encoded)
        with peare_lib.open_file(str(archive)) as opener:
            rows = opener.normalized_tree()
            assert opener.container_format() == "LZIP"
            assert len(rows) == 1 and rows[0]["sha256"] == expected_hash

        if level == 6 and winrar:
            output_dir = tmp_path / "winrar-lzip"
            output_dir.mkdir()
            result = subprocess.run(
                [winrar, "x", "-ibck", "-y", str(archive),
                 str(output_dir) + "\\"], capture_output=True,
                text=True, timeout=60)
            assert result.returncode == 0, result.stdout + result.stderr
            assert (output_dir / archive.stem).read_bytes() == expected

    gui_output = tmp_path / "lzip-gui-preview"
    gui_check = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
         "--exe", str(peare_gui_path), "--case", "lzip-writer-level6",
         str(gui_archive), "created-level-6", "--output-dir", str(gui_output)],
        capture_output=True, text=True, timeout=120, check=False)
    assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr

def test_lzip_writer_concatenates_selected_inputs(writer_lib, peare_lib, tmp_path):
    sources = [ROOT / "README.md", ROOT / "docs" / "SUPPORTED_FORMATS.md"]
    expected = [source.read_bytes() for source in sources]
    inputs = (_WriterInputFile * len(sources))(*[
        _WriterInputFile(str(source).encode(), source.name.encode())
        for source in sources])
    archive = tmp_path / "concatenated.lz"
    options = _WriterCreateOptions(b"lzip", b"level-6", None)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            inputs, len(sources))
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    encoded = archive.read_bytes()
    offset = 0
    decoded_members = []
    while offset < len(encoded):
        assert encoded[offset:offset + 4] == b"LZIP"
        assert encoded[offset + 4] == 1
        coded_size = encoded[offset + 5]
        exponent = coded_size & 0x1f
        fraction = coded_size >> 5
        dictionary = (1 << exponent) - ((1 << exponent) >> 4) * fraction
        decoder = lzma.LZMADecompressor(
            format=lzma.FORMAT_RAW,
            filters=[{"id": lzma.FILTER_LZMA1, "dict_size": dictionary,
                      "lc": 3, "lp": 0, "pb": 2}])
        member_data = decoder.decompress(encoded[offset + 6:])
        compressed_size = len(encoded[offset + 6:]) - len(decoder.unused_data)
        trailer_offset = offset + 6 + compressed_size
        checksum, data_size, member_size = struct.unpack_from(
            "<IQQ", encoded, trailer_offset)
        assert checksum == zlib.crc32(member_data)
        assert data_size == len(member_data)
        assert member_size == compressed_size + 26
        decoded_members.append(member_data)
        offset = trailer_offset + 20
    assert decoded_members == expected
    with peare_lib.open_file(str(archive)) as opener:
        assert opener.container_format() == "LZIP"
        rows = opener.normalized_tree()
        assert len(rows) == 1
        assert rows[0]["sha256"] == hashlib.sha256(b"".join(expected)).hexdigest()

@pytest.mark.parametrize("variant", [
    "classic", "classic-encoded-name", "base64", "base64-encoded-name"])
def test_uuencode_writer_roundtrip_all_headers(
        writer_lib, peare_lib, peare_gui_path, tmp_path, variant):
    assert writer_lib.peare_writer_create_format_count() == 30
    formats = (_WriterFormat * 30)()
    assert all(writer_lib.peare_writer_get_create_format(
        index, C.byref(formats[index])) == 0 for index in range(len(formats)))
    assert {item.id for item in formats} >= {b"uuencode", b"ihex"}
    assert writer_lib.peare_writer_create_variant_count(b"uuencode") == 4
    source = ROOT / "README.md"
    payload = source.read_bytes()
    expected_hash = hashlib.sha256(payload).hexdigest()
    entry_name = "Peare UU sample.txt"
    archive = tmp_path / f"created-{variant}.uue"
    source_item = _WriterInputFile(str(source).encode(), entry_name.encode())
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to(
            writer, str(archive).encode(), b"uuencode", variant.encode(),
            0, C.byref(source_item), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    lines = archive.read_bytes().splitlines()
    assert lines[0].startswith(b"begin")
    if variant.startswith("base64"):
        assert lines[-1] == b"===="
        decoded = base64.b64decode(b"".join(lines[1:-1]), validate=True)
    else:
        assert lines[-1] == b"end"
        decoded = b"".join(binascii.a2b_uu(line) for line in lines[1:-1])
    assert decoded == payload
    if variant.endswith("encoded-name"):
        assert base64.b64decode(lines[0].split()[-1]) == entry_name.encode()

    with peare_lib.open_file(str(archive)) as opener:
        assert opener.container_format() == "UUE"
        rows = opener.normalized_tree()
        assert len(rows) == 1 and rows[0]["sha256"] == expected_hash
        tree_path = rows[0]["path"]

    if variant == "base64":
        gui_output = tmp_path / "uue-gui-preview"
        gui_check = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
             "--exe", str(peare_gui_path), "--case", "uue-writer-base64",
             str(archive), tree_path, "--output-dir", str(gui_output)],
            capture_output=True, text=True, timeout=120, check=False)
        assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr


@pytest.mark.parametrize("variant", ["classic", "base64"])
def test_uuencode_writer_empty_input_and_multiple_files(
        writer_lib, peare_lib, tmp_path, variant):
    empty_archive = tmp_path / f"empty-{variant}.uue"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"uuencode", variant.encode(), str(empty_archive).encode()) == 0
        assert writer_lib.peare_writer_create_empty_file(writer, b"empty.bin") == 0
        assert writer_lib.peare_writer_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert empty_archive.read_bytes().splitlines()[-1] == (
        b"====" if variant == "base64" else b"end")
    with peare_lib.open_file(str(empty_archive)) as opener:
        rows = opener.normalized_tree()
        assert len(rows) == 1 and rows[0]["size"] == 0

    source = ROOT / "README.md"
    inputs = (_WriterInputFile * 2)(
        _WriterInputFile(str(source).encode(), b"one.txt"),
        _WriterInputFile(str(source).encode(), b"two.txt"))
    archive = tmp_path / "multiple.uue"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to(
            writer, str(archive).encode(), b"uuencode", variant.encode(),
            0, inputs, 2)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    lines = archive.read_bytes().splitlines()
    assert len([line for line in lines if line.startswith(b"begin")]) == 2
    if variant == "base64":
        assert lines.count(b"====") == 2
    else:
        assert lines.count(b"end") == 2
    with peare_lib.open_file(str(archive)) as opener:
        assert opener.container_format() == "UUE"
        rows = opener.normalized_tree()
        assert len(rows) == 2


def _decode_classic_uue_members(data):
    members = {}
    lines = data.splitlines()
    index = 0
    while index < len(lines):
        header = lines[index]
        index += 1
        if not header.startswith(b"begin "):
            continue
        parts = header.split(b" ", 2)
        assert len(parts) == 3
        name = parts[2].decode("ascii")
        payload = bytearray()
        while index < len(lines) and lines[index] != b"end":
            payload.extend(binascii.a2b_uu(lines[index]))
            index += 1
        assert index < len(lines)
        index += 1
        assert name not in members
        members[name] = bytes(payload)
    return members


def test_uuencode_writer_full_cud_on_libarchive_sample(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    sample_root = Path(os.environ.get(
        "PEARE_COMPRESS_SAMPLE_ROOT",
        Path.home() / "Documents" / "inerente a" / "Peare" /
        "compress-samples" / "libarchive"))
    sample = sample_root / "test_compat_mac-1.tar.Z.uu"
    expected_payload = sample_root / "test_compat_mac-1.tar.Z"
    if not sample.is_file() or not expected_payload.is_file():
        pytest.skip("Missing independent libarchive UUencode sample pair")
    archive = tmp_path / sample.name
    shutil.copyfile(sample, archive)
    original_bytes = sample.read_bytes()
    initial_members = _decode_classic_uue_members(original_bytes)
    assert initial_members == {"test_compat_mac-1.tar.Z": expected_payload.read_bytes()}
    archive_path = str(archive).encode()
    assert writer_lib.peare_writer_can_add_files(archive_path) == 1
    assert writer_lib.peare_writer_update_target_count(archive_path) == 1
    capabilities = C.c_uint32()
    assert writer_lib.peare_writer_get_update_capabilities(
        archive_path, 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3B

    added = ROOT / "README.md"
    replacement = ROOT / "LICENSE"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, archive_path) == 0
        assert writer_lib.peare_writer_add_file(
            writer, str(added).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    first_members = _decode_classic_uue_members(archive.read_bytes())
    assert first_members == {
        "test_compat_mac-1.tar.Z": expected_payload.read_bytes(),
        "added.txt": added.read_bytes(),
    }

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, archive_path) == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"test_compat_mac-1.tar.Z", b"renamed.tar.Z") == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"renamed.tar.Z") == 0
        assert writer_lib.peare_writer_delete_entry(writer, b"added.txt", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    final_members = _decode_classic_uue_members(archive.read_bytes())
    assert final_members == {"renamed.tar.Z": replacement.read_bytes()}
    with peare_lib.open_file(str(archive)) as opener:
        rows = opener.normalized_tree()
        assert len(rows) == 1
        assert rows[0]["path"].endswith("renamed.tar.Z")
        assert rows[0]["sha256"] == hashlib.sha256(replacement.read_bytes()).hexdigest()
    assert sample.read_bytes() == original_bytes

    for view in ("tree", "details"):
        scenario = {
            "view": view,
            "operations": [
                {"type": "add_files", "sources": [str(added)],
                 "expect_present": "README.md"},
                {"type": "rename", "path": "test_compat_mac-1.tar.Z",
                 "name": "renamed.tar.Z", "expect_present": "renamed.tar.Z"},
                {"type": "replace", "path": "renamed.tar.Z",
                 "source": str(replacement), "expect_present": "renamed.tar.Z"},
                {"type": "delete", "path": "README.md",
                 "expect_absent": "README.md"},
            ],
        }
        scenario_path = tmp_path / f"uue-gui-{view}.json"
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        output_dir = tmp_path / f"uue-gui-{view}"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
             "--exe", str(peare_gui_path),
             "--image", str(sample), "--scenario", str(scenario_path),
             "--output-dir", str(output_dir)],
            capture_output=True, text=True, timeout=180)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads((output_dir / "writer-test-report.json").read_text(
            encoding="utf-8"))
        assert len(report["operations"]) == len(scenario["operations"])
        assert all(item["passed"] for item in report["operations"])
    assert sample.read_bytes() == original_bytes


def _decode_ihex_records(path):
    address_base = 0
    payload = {}
    saw_eof = False
    for line in path.read_text(encoding="ascii").splitlines():
        assert line.startswith(":")
        record = bytes.fromhex(line[1:])
        assert len(record) == record[0] + 5
        assert sum(record) & 0xff == 0
        address = (record[1] << 8) | record[2]
        kind = record[3]
        data = record[4:-1]
        if kind == 0:
            start = address_base + address
            assert all(start + index not in payload for index in range(len(data)))
            payload.update({start + index: value for index, value in enumerate(data)})
        elif kind == 1:
            assert address == 0 and not data
            saw_eof = True
            break
        elif kind == 4:
            assert address == 0 and len(data) == 2
            address_base = int.from_bytes(data, "big") << 16
        else:
            raise AssertionError(f"Unexpected Intel HEX record type: {kind}")
    assert saw_eof
    if not payload:
        return b""
    assert sorted(payload) == list(range(len(payload)))
    return bytes(payload[index] for index in range(len(payload)))


@pytest.mark.parametrize("variant", ["record-16", "record-32", "record-64"])
def test_ihex_writer_variants_with_7zip(
        writer_lib, peare_lib, peare_gui_path, tmp_path, variant):
    source = ROOT / ("README.md" if variant == "record-16" else
                     "docs/SUPPORTED_FORMATS.md" if variant == "record-32" else
                     "src/writer/api/Api.cpp")
    payload = source.read_bytes()
    expected_hash = hashlib.sha256(payload).hexdigest()
    archive = tmp_path / f"created-{variant}.hex"
    source_item = _WriterInputFile(str(source).encode(), source.name.encode())
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to(
            writer, str(archive).encode(), b"ihex", variant.encode(),
            0, C.byref(source_item), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    lines = archive.read_text(encoding="ascii").splitlines()
    records = [bytes.fromhex(line[1:]) for line in lines]
    assert all((sum(record) & 0xff) == 0 for record in records)
    assert _decode_ihex_records(archive) == payload
    expected_record_size = int(variant.removeprefix("record-"))
    data_sizes = [record[0] for record in records if record[3] == 0]
    assert max(data_sizes) <= expected_record_size

    with peare_lib.open_file(str(archive)) as opener:
        assert opener.container_format() == "INTEL_HEX"
        rows = opener.normalized_tree()
        assert len(rows) == 1 and rows[0]["sha256"] == expected_hash
        tree_path = rows[0]["path"]

    if variant == "record-64":
        assert any(record[3] == 4 for record in records)
    if variant == "record-16":
        seven_zip = _seven_zip_executable()
        assert seven_zip
        destination = tmp_path / "7zip-ihex"
        destination.mkdir()
        result = subprocess.run(
            [seven_zip, "x", str(archive), f"-o{destination}", "-y"],
            capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        extracted = [path for path in destination.rglob("*") if path.is_file()]
        assert len(extracted) == 1 and extracted[0].read_bytes() == payload
        gui_output = tmp_path / "ihex-gui-preview"
        gui_check = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
             "--exe", str(peare_gui_path), "--case", "ihex-writer-record16",
             str(archive), tree_path, "--output-dir", str(gui_output)],
            capture_output=True, text=True, timeout=120, check=False)
        assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr


def test_ihex_writer_requires_one_input(writer_lib, tmp_path):
    source = ROOT / "README.md"
    inputs = (_WriterInputFile * 2)(
        _WriterInputFile(str(source).encode(), b"one.bin"),
        _WriterInputFile(str(source).encode(), b"two.bin"))
    output = tmp_path / "multiple.hex"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to(
            writer, str(output).encode(), b"ihex", b"record-16",
            0, inputs, 2)
        assert status != 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert not output.exists()


def test_ihex_writer_empty_input(writer_lib, peare_lib, tmp_path):
    output = tmp_path / "empty.hex"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"ihex", b"record-16", str(output).encode()) == 0
        assert writer_lib.peare_writer_create_empty_file(writer, b"empty.bin") == 0
        assert writer_lib.peare_writer_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _decode_ihex_records(output) == b""
    with peare_lib.open_file(str(output)) as opener:
        rows = opener.normalized_tree()
        assert len(rows) == 1 and rows[0]["size"] == 0


def _ar_members(path):
    data = path.read_bytes()
    assert data.startswith(b"!<arch>\n")
    offset = 8
    members = {}
    while offset < len(data):
        header = data[offset:offset + 60]
        assert len(header) == 60 and header[58:60] == b"`\n"
        name = header[:16].decode("ascii").strip().rstrip("/")
        size = int(header[48:58].decode("ascii").strip())
        offset += 60
        members[name] = data[offset:offset + size]
        offset += size + (size & 1)
    assert offset == len(data)
    return members


def _deb_tar_payload(data, codec):
    if codec == "raw":
        return data
    if codec == "gzip":
        return gzip.decompress(data)
    if codec == "xz":
        return lzma.decompress(data, format=lzma.FORMAT_XZ)
    if codec == "zstd":
        with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(data)) as reader:
            return reader.read()
    if codec == "bzip2":
        return bz2.decompress(data)
    if codec == "lzma":
        return lzma.decompress(data, format=lzma.FORMAT_ALONE)
    raise AssertionError(codec)


def _tar_members(data):
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
        return {member.name.rstrip("/"): archive.extractfile(member).read()
                for member in archive.getmembers() if member.isfile()}


@pytest.mark.parametrize("control_codec", ["raw", "gzip", "xz", "zstd"])
@pytest.mark.parametrize("data_codec", ["raw", "gzip", "xz", "zstd", "bzip2", "lzma"])
def test_deb_writer_all_compression_combinations(
        writer_lib, peare_lib, peare_gui_path, tmp_path, control_codec, data_codec):
    source = ROOT / "README.md"
    payload = source.read_bytes()
    variant = f"control-{control_codec}+data-{data_codec}"
    output = tmp_path / f"package-{control_codec}-{data_codec}.deb"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_variant_count(b"deb") == 24
        if control_codec == "raw" and data_codec == "raw":
            source_item = _WriterInputFile(
                str(source).encode(), b"usr/share/peare/README.md")
            status = writer_lib.peare_writer_add_files_to(
                writer, str(output).encode(), b"deb", variant.encode(), 0,
                C.byref(source_item), 1)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
        else:
            status = writer_lib.peare_writer_create_begin_variant(
                writer, b"deb", variant.encode(), str(output).encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            assert writer_lib.peare_writer_create_directory(
                writer, b"usr/share/peare") == 0
            assert writer_lib.peare_writer_create_file(
                writer, str(source).encode(), b"usr/share/peare/README.md") == 0
            assert writer_lib.peare_writer_finish(writer) == 0, \
                writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    members = _ar_members(output)
    suffixes = {"raw": "", "gzip": ".gz", "xz": ".xz", "zstd": ".zst",
                "bzip2": ".bz2", "lzma": ".lzma"}
    assert list(members) == ["debian-binary", f"control.tar{suffixes[control_codec]}",
                             f"data.tar{suffixes[data_codec]}"]
    assert members["debian-binary"] == b"2.0\n"
    control_name = next(name for name in members if name.startswith("control.tar"))
    data_name = next(name for name in members if name.startswith("data.tar"))
    control = _tar_members(_deb_tar_payload(members[control_name], control_codec))
    data = _tar_members(_deb_tar_payload(members[data_name], data_codec))
    assert control["control"].startswith(b"Package: peare-generated\n")
    assert data["usr/share/peare/README.md"] == payload

    with peare_lib.open_file(str(output)) as opener:
        assert opener.container_format() == "DEB"
        rows = opener.normalized_tree()
    leaf = next(row for row in rows if row["path"].endswith("usr/share/peare/README.md"))
    assert leaf["sha256"] == hashlib.sha256(payload).hexdigest()
    if control_codec == "raw" and data_codec == "raw":
        seven_zip = _seven_zip_executable()
        assert seven_zip
        check = subprocess.run([seven_zip, "t", str(output)], capture_output=True,
                               text=True, timeout=60)
        assert check.returncode == 0, check.stdout + check.stderr
        gui_output = tmp_path / "gui-preview"
        gui_check = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
             "--exe", str(peare_gui_path), "--case", "deb-writer-raw-raw",
             str(output), leaf["path"], "--output-dir", str(gui_output)],
            capture_output=True, text=True, timeout=120, check=False)
        assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr


def test_deb_existing_package_full_cud(writer_lib, peare_lib, peare_gui_path,
                                      tmp_path):
    sample_root = os.environ.get("PEARE_DEB_SAMPLE_ROOT")
    if not sample_root:
        pytest.skip("Set PEARE_DEB_SAMPLE_ROOT to the local Debian sample directory")
    sample = Path(sample_root) / "hello_2.12.3-1_amd64.deb"
    if not sample.is_file():
        pytest.skip(f"Missing Debian package sample: {sample}")
    source_hash = hashlib.sha256(sample.read_bytes()).digest()
    original_members = _ar_members(sample)
    control_name = next(name for name in original_members
                        if name.startswith("control.tar"))
    data_name = next(name for name in original_members if name.startswith("data.tar"))
    data_codec = {"data.tar": "raw", "data.tar.gz": "gzip", "data.tar.xz": "xz",
                  "data.tar.zst": "zstd", "data.tar.bz2": "bzip2",
                  "data.tar.lzma": "lzma"}[data_name]
    image = tmp_path / sample.name
    shutil.copy2(sample, image)
    target = str(image).encode()
    assert writer_lib.peare_writer_can_add_files(target) == 1
    assert writer_lib.peare_writer_update_target_count(target) == 1
    capabilities = C.c_uint32()
    assert writer_lib.peare_writer_get_update_capabilities(
        target, 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3F

    payload = ROOT / "README.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, target) == 0
        assert writer_lib.peare_writer_create_directory(writer, b"usr/share/peare-cud") == 0
        assert writer_lib.peare_writer_add_file(
            writer, str(payload).encode(), b"usr/share/peare-cud/added.txt") == 0
        assert writer_lib.peare_writer_create_empty_file(
            writer, b"usr/share/peare-cud/empty.txt") == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(payload).encode(), b"usr/bin/hello") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"usr/share/peare-cud/added.txt",
            b"usr/share/peare-cud/renamed.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"usr/share/peare-cud/renamed.txt", 0) == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"usr/share/peare-cud", 1) == 0
        status = writer_lib.peare_writer_update_finish(writer)
        assert status == 0, writer_lib.peare_writer_last_error(writer).decode()
    finally:
        writer_lib.peare_writer_destroy(writer)

    assert hashlib.sha256(sample.read_bytes()).digest() == source_hash
    updated_members = _ar_members(image)
    assert updated_members[control_name] == original_members[control_name]
    assert updated_members["debian-binary"] == original_members["debian-binary"]
    updated_data = _tar_members(_deb_tar_payload(updated_members[data_name], data_codec))
    normalized_data = {name.removeprefix("./"): content
                       for name, content in updated_data.items()}
    assert normalized_data["usr/bin/hello"] == payload.read_bytes()
    assert "usr/share/peare-cud/renamed.txt" not in normalized_data
    assert "usr/share/peare-cud/empty.txt" not in normalized_data

    seven_zip = _seven_zip_executable()
    assert seven_zip
    check = subprocess.run([seven_zip, "t", str(image)], capture_output=True,
                           text=True, timeout=120)
    assert check.returncode == 0, check.stdout + check.stderr

    for view in ("tree", "details"):
        gui_copy = tmp_path / f"gui-{view}.deb"
        shutil.copy2(sample, gui_copy)
        scenario = {
            "view": view,
            "operations": [
                {"type": "create_directory", "name": "gui-cud",
                 "expect_present": "data.tar.xz/data.tar/gui-cud"},
                {"type": "add_files", "destination": "gui-cud",
                 "sources": [str(payload)],
                 "expect_present": "data.tar.xz/data.tar/gui-cud/README.md"},
                {"type": "create_file",
                 "parent": "data.tar.xz/data.tar/gui-cud", "name": "empty.txt",
                 "expect_present": "data.tar.xz/data.tar/gui-cud/empty.txt"},
                {"type": "replace", "path": "data.tar.xz/data.tar/usr/bin/hello",
                 "source": str(payload),
                 "expect_present": "data.tar.xz/data.tar/usr/bin/hello"},
                {"type": "rename",
                 "path": "data.tar.xz/data.tar/gui-cud/README.md",
                 "name": "renamed.txt",
                 "expect_present": "data.tar.xz/data.tar/gui-cud/renamed.txt"},
                {"type": "delete",
                 "path": "data.tar.xz/data.tar/gui-cud/renamed.txt",
                 "expect_absent": "data.tar.xz/data.tar/gui-cud/renamed.txt"},
                {"type": "delete",
                 "path": "data.tar.xz/data.tar/gui-cud/empty.txt",
                 "expect_absent": "data.tar.xz/data.tar/gui-cud/empty.txt"},
                {"type": "delete", "path": "data.tar.xz/data.tar/gui-cud",
                 "expect_absent": "data.tar.xz/data.tar/gui-cud"},
            ],
        }
        scenario_path = tmp_path / f"deb-gui-{view}.json"
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        output_dir = tmp_path / f"deb-gui-{view}"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
             "--exe", str(peare_gui_path), "--image", str(gui_copy),
             "--scenario", str(scenario_path), "--output-dir", str(output_dir)],
            capture_output=True, text=True, timeout=240)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads((output_dir / "writer-test-report.json").read_text(
            encoding="utf-8"))
        assert len(report["operations"]) == len(scenario["operations"])
        assert all(item["passed"] for item in report["operations"])
        gui_result = output_dir / "writer-test-copy.deb"
        gui_check = subprocess.run([seven_zip, "t", str(gui_result)],
                                   capture_output=True, text=True, timeout=120)
        assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr
        with peare_lib.open_file(str(gui_result)) as opener:
            gui_rows = opener.normalized_tree()
        gui_leaf = next(row for row in gui_rows
                        if row["path"].endswith("usr/bin/hello"))
        assert gui_leaf["sha256"] == hashlib.sha256(payload.read_bytes()).hexdigest()


def _rpm_header(data, offset):
    assert data[offset:offset + 8] == b"\x8e\xad\xe8\x01\0\0\0\0"
    count, store_size = struct.unpack_from(">II", data, offset + 8)
    end_index = offset + 16 + count * 16
    end = end_index + store_size
    return data[offset:end], end


def _rpm_header_tags(data, offset):
    count, store_size = struct.unpack_from(">II", data, offset + 8)
    index_offset = offset + 16
    store_offset = index_offset + count * 16
    tags = {}
    for index in range(count):
        tag, kind, value_offset, items = struct.unpack_from(
            ">IIII", data, index_offset + index * 16)
        size = {3: 2, 4: 4, 7: 1}.get(kind, 0)
        if kind in (3, 4):
            values = [int.from_bytes(data[store_offset + value_offset + i * size:
                                         store_offset + value_offset + (i + 1) * size],
                                     "big") for i in range(items)]
            tags[tag] = values
        elif kind == 7:
            tags[tag] = data[store_offset + value_offset:
                             store_offset + value_offset + items]
        elif kind in (6, 8, 9):
            start = store_offset + value_offset
            end = data.index(b"\0", start)
            tags[tag] = data[start:end + 1]
    assert store_offset + store_size <= len(data)
    return tags


def _rpm_cpio_crc(data):
    offset = 0
    files = {}
    while True:
        header = data[offset:offset + 110]
        assert header[:6] in (b"070701", b"070702")
        has_checksum = header[:6] == b"070702"
        values = [int(header[i:i + 8], 16) for i in range(6, 110, 8)]
        file_size, name_size, expected_sum = values[6], values[11], values[12]
        offset += 110
        name = data[offset:offset + name_size - 1].decode("utf-8")
        offset = (offset + name_size + 3) & ~3
        payload = data[offset:offset + file_size]
        offset = (offset + file_size + 3) & ~3
        if name == "TRAILER!!!":
            break
        if has_checksum:
            assert sum(payload) & 0xffffffff == expected_sum
        else:
            assert expected_sum == 0
        files[name.removeprefix("./")] = payload
    return files


def _rpm_string_array(data, header_offset, tag_id):
    count, store_size = struct.unpack_from(">II", data, header_offset + 8)
    store_offset = header_offset + 16 + count * 16
    for index in range(count):
        tag, kind, value_offset, items = struct.unpack_from(
            ">IIII", data, header_offset + 16 + index * 16)
        if tag != tag_id:
            continue
        assert kind in (6, 8, 9)
        cursor = store_offset + value_offset
        values = []
        for _ in range(items):
            end = data.index(b"\0", cursor)
            values.append(data[cursor:end])
            cursor = end + 1
        return values
    raise AssertionError(f"Missing RPM tag {tag_id}")


@pytest.mark.parametrize("sample_name", [
    "hello-2.12.1-6.fc42.x86_64.rpm",
    "hello-2.3-1.el3.rf.x86_64.rpm",
])
def test_rpm_existing_package_full_cud(writer_lib, peare_lib, peare_gui_path,
                                      tmp_path, sample_name):
    sample_root = os.environ.get("PEARE_RPM_SAMPLE_ROOT")
    if not sample_root:
        pytest.skip("Set PEARE_RPM_SAMPLE_ROOT to the local RPM sample directory")
    sample = Path(sample_root) / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing RPM package sample: {sample}")
    original = sample.read_bytes()
    image = tmp_path / sample.name
    shutil.copy2(sample, image)
    target = str(image).encode()
    assert writer_lib.peare_writer_can_add_files(target) == 1
    assert writer_lib.peare_writer_update_target_count(target) == 1
    capabilities = C.c_uint32()
    assert writer_lib.peare_writer_get_update_capabilities(
        target, 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3F

    payload = ROOT / "README.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, target) == 0
        assert writer_lib.peare_writer_create_directory(writer, b"usr/share/peare-cud") == 0
        assert writer_lib.peare_writer_add_file(
            writer, str(payload).encode(), b"usr/share/peare-cud/added.txt") == 0
        assert writer_lib.peare_writer_create_empty_file(
            writer, b"usr/share/peare-cud/empty.txt") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"usr/share/peare-cud/added.txt",
            b"usr/share/peare-cud/renamed.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"usr/share/peare-cud/renamed.txt", 0) == 0
        assert writer_lib.peare_writer_create_directory(
            writer, b"usr/share/peare-disposable") == 0
        assert writer_lib.peare_writer_add_file(
            writer, str(payload).encode(), b"usr/share/peare-disposable/remove.txt") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"usr/share/peare-disposable", 1) == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(payload).encode(), b"usr/bin/hello") == 0
        status = writer_lib.peare_writer_update_finish(writer)
        assert status == 0, writer_lib.peare_writer_last_error(writer).decode()
    finally:
        writer_lib.peare_writer_destroy(writer)

    assert sample.read_bytes() == original
    data = image.read_bytes()
    assert data[:96] == original[:96]
    original_signature, original_sig_end = _rpm_header(original, 96)
    original_main_offset = (original_sig_end + 7) & ~7
    original_main, _ = _rpm_header(original, original_main_offset)
    original_tags = _rpm_header_tags(original, original_main_offset)
    signature, sig_end = _rpm_header(data, 96)
    main_offset = (sig_end + 7) & ~7
    main_header, payload_offset = _rpm_header(data, main_offset)
    main_tags = _rpm_header_tags(data, main_offset)
    signature_tags = _rpm_header_tags(data, 96)
    payload_bytes = data[payload_offset:]
    assert signature_tags[1000] == [len(main_header) + len(payload_bytes)]
    assert signature_tags[1004] == hashlib.md5(main_header + payload_bytes).digest()
    for tag in (1000, 1001, 1002, 1022, 1124, 1125):
        assert main_tags[tag] == original_tags[tag]
    compressor = main_tags[1125].rstrip(b"\0")
    if compressor == b"gzip":
        cpio = gzip.decompress(payload_bytes)
    elif compressor == b"bzip2":
        cpio = bz2.decompress(payload_bytes)
    elif compressor == b"xz":
        cpio = lzma.decompress(payload_bytes, format=lzma.FORMAT_XZ)
    elif compressor == b"lzma":
        cpio = lzma.decompress(payload_bytes, format=lzma.FORMAT_ALONE)
    elif compressor == b"zstd":
        with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(payload_bytes)) as reader:
            cpio = reader.read()
    else:
        cpio = payload_bytes
    files = _rpm_cpio_crc(cpio)
    assert files["usr/bin/hello"] == payload.read_bytes()
    assert files["usr/share/peare-cud/empty.txt"] == b""
    assert "usr/share/peare-cud/renamed.txt" not in files
    assert not any(name.startswith("usr/share/peare-disposable/") for name in files)
    directories = _rpm_string_array(data, main_offset, 1118)
    basenames = _rpm_string_array(data, main_offset, 1117)
    paths = [directories[index] + name
             for index, name in zip(main_tags[1116], basenames)]
    assert b"/usr/bin/hello" in paths
    assert b"/usr/share/peare-cud/empty.txt" in paths
    assert not any(b"peare-disposable" in path for path in paths)
    with peare_lib.open_file(str(image)) as opener:
        assert opener.container_format() == "RPM"
        rows = opener.normalized_tree()
    leaf = next(row for row in rows if row["path"].endswith("usr/bin/hello"))
    assert leaf["sha256"] == hashlib.sha256(payload.read_bytes()).hexdigest()
    seven_zip = _seven_zip_executable()
    assert seven_zip
    check = subprocess.run([seven_zip, "t", str(image)], capture_output=True,
                           text=True, timeout=120)
    assert check.returncode == 0, check.stdout + check.stderr

    for view in ("tree", "details"):
        gui_copy = tmp_path / f"gui-{view}.rpm"
        shutil.copy2(sample, gui_copy)
        scenario = {
            "view": view,
            "operations": [
                {"type": "create_directory", "name": "gui-cud",
                 "expect_present": "gui-cud"},
                {"type": "add_files", "destination": "gui-cud",
                 "sources": [str(payload)],
                 "expect_present": "gui-cud/README.md"},
                {"type": "create_file", "parent": "gui-cud", "name": "empty.txt",
                 "expect_present": "gui-cud/empty.txt"},
                {"type": "replace", "path": "usr/bin/hello",
                 "source": str(payload), "expect_present": "usr/bin/hello"},
                {"type": "rename", "path": "gui-cud/README.md",
                 "name": "renamed.txt", "expect_present": "gui-cud/renamed.txt"},
                {"type": "delete", "path": "gui-cud/renamed.txt",
                 "expect_absent": "gui-cud/renamed.txt"},
                {"type": "delete", "path": "gui-cud/empty.txt",
                 "expect_absent": "gui-cud/empty.txt"},
                {"type": "delete", "path": "gui-cud",
                 "expect_absent": "gui-cud"},
            ],
        }
        scenario_path = tmp_path / f"rpm-gui-{view}.json"
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        output_dir = tmp_path / f"rpm-gui-{view}"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
             "--exe", str(peare_gui_path), "--image", str(gui_copy),
             "--scenario", str(scenario_path), "--output-dir", str(output_dir)],
            capture_output=True, text=True, timeout=240)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads((output_dir / "writer-test-report.json").read_text(
            encoding="utf-8"))
        assert len(report["operations"]) == len(scenario["operations"])
        assert all(item["passed"] for item in report["operations"])
        gui_result = output_dir / "writer-test-copy.rpm"
        with peare_lib.open_file(str(gui_result)) as opener:
            gui_rows = opener.normalized_tree()
        gui_leaf = next(row for row in gui_rows
                        if row["path"].endswith("usr/bin/hello"))
        assert gui_leaf["sha256"] == hashlib.sha256(payload.read_bytes()).hexdigest()


@pytest.mark.parametrize("codec", ["none", "gzip", "bzip2", "xz", "zstd", "lzma"])
def test_rpm_writer_payload_codecs(writer_lib, peare_lib, peare_gui_path, tmp_path, codec):
    source = ROOT / "README.md"
    expected = source.read_bytes()
    output = tmp_path / f"peare-{codec}.rpm"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_variant_count(b"rpm") == 6
        status = writer_lib.peare_writer_create_begin_variant(
            writer, b"rpm", codec.encode(), str(output).encode())
        assert status == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"usr/share/peare/README.md") == 0
        status = writer_lib.peare_writer_finish(writer)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    data = output.read_bytes()
    assert data[:4] == b"\xed\xab\xee\xdb" and data[4] == 3
    signature, sig_end = _rpm_header(data, 96)
    assert all(byte == 0 for byte in data[sig_end:(sig_end + 7) & ~7])
    main_header, payload_offset = _rpm_header(data, (sig_end + 7) & ~7)
    payload = data[payload_offset:]
    assert len(payload) > 0 and len(signature) > 0 and len(main_header) > 0
    signature_tags = _rpm_header_tags(data, 96)
    assert signature_tags[1000] == [len(main_header) + len(payload)]
    assert signature_tags[1004] == hashlib.md5(main_header + payload).digest()
    main_tags = _rpm_header_tags(data, (sig_end + 7) & ~7)
    assert main_tags[1000] == b"peare-generated\0"
    assert main_tags[1124] == b"cpio\0"
    assert main_tags[1125] == codec.encode() + b"\0"
    if codec == "none":
        cpio = payload
    elif codec == "gzip":
        cpio = gzip.decompress(payload)
    elif codec == "bzip2":
        cpio = bz2.decompress(payload)
    elif codec == "xz":
        cpio = lzma.decompress(payload, format=lzma.FORMAT_XZ)
    elif codec == "lzma":
        cpio = lzma.decompress(payload, format=lzma.FORMAT_ALONE)
    else:
        with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(payload)) as reader:
            cpio = reader.read()
    assert _rpm_cpio_crc(cpio)["usr/share/peare/README.md"] == expected

    with peare_lib.open_file(str(output)) as opener:
        assert opener.container_format() == "RPM"
        rows = opener.normalized_tree()
    leaf = next(row for row in rows if row["path"].endswith("usr/share/peare/README.md"))
    assert leaf["sha256"] == hashlib.sha256(expected).hexdigest()
    if codec == "gzip":
        gui = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
             "--exe", str(peare_gui_path), "--case", "rpm-writer-gzip",
             str(output), leaf["path"], "--output-dir", str(tmp_path / "gui")],
            capture_output=True, text=True, timeout=120)
        assert gui.returncode == 0, gui.stdout + gui.stderr
    seven_zip = _seven_zip_executable()
    if seven_zip and codec == "gzip":
        result = subprocess.run([seven_zip, "t", str(output)], capture_output=True,
                                text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr

@pytest.mark.parametrize(("sample_name", "tar_root"), [
    ("docs.tgz", "archive.tar"), ("docs.tbz2", "writer-test-copy.tbz2"),
    ("docs.tar.zst", "writer-test-copy")])
@pytest.mark.parametrize("view", ["tree", "details"])
def test_compressed_tar_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                      tmp_path, sample_name, tar_root, view):
    sample_root = (ROOT / "tests" / "formats" / "zstd" / "fixtures"
                   if sample_name == "docs.tar.zst" else
                   ROOT / "tests" / "formats" / "tar-compressed" / "fixtures")
    sample = sample_root / sample_name
    prefix = f"{tar_root}/"
    sources = [ROOT / "README.md", ROOT / "AGENTS.md"]
    scenario = {
        "view": view,
        "target_index": 0,
        "operations": [
            {"type": "create_directory", "name": "PeareUiCud"},
            {"type": "add_files", "view_path": tar_root,
             "destination": "PeareUiCud",
             "sources": [str(source) for source in sources],
             "expect_present": f"{prefix}PeareUiCud/README.md"},
            {"type": "replace",
             "path": f"{prefix}PeareUiCud/README.md",
             "view_path": f"{prefix}PeareUiCud",
             "source": str(ROOT / "docs" / "SUPPORTED_FORMATS.md"),
             "expect_present": f"{prefix}PeareUiCud/README.md"},
            {"type": "rename",
             "path": f"{prefix}PeareUiCud/README.md",
             "view_path": f"{prefix}PeareUiCud", "name": "renamed.txt",
             "expect_present": f"{prefix}PeareUiCud/renamed.txt",
             "expect_absent": f"{prefix}PeareUiCud/README.md"},
            {"type": "create_file", "parent": f"{prefix}PeareUiCud",
             "view_path": f"{prefix}PeareUiCud", "name": "delete.txt",
             "expect_present": f"{prefix}PeareUiCud/delete.txt"},
            {"type": "delete", "path": f"{prefix}PeareUiCud/delete.txt",
             "view_path": f"{prefix}PeareUiCud",
             "expect_absent": f"{prefix}PeareUiCud/delete.txt"},
        ],
    }
    scenario_path = tmp_path / f"tar-compressed-gui-{sample_name}-{view}.json"
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
    output_dir = tmp_path / f"tar-compressed-gui-{sample_name}-{view}"
    runner = ROOT / "tests" / "run_gui_writer_tests.py"
    result = subprocess.run(
        [sys.executable, str(runner), "--exe", str(peare_gui_path),
         "--image", str(sample), "--scenario", str(scenario_path),
         "--output-dir", str(output_dir)],
        capture_output=True, text=True, timeout=240)
    assert result.returncode == 0, result.stdout + result.stderr
    report = json.loads((output_dir / "writer-test-report.json").read_text(
        encoding="utf-8"))
    assert all(operation["passed"] for operation in report["operations"])
    image = report["image"]
    opened = peare_lib.open_file(image)
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        for source in sources:
            name = source.name
            if name == "README.md":
                assert rows[f"{prefix}PeareUiCud/renamed.txt"] == hashlib.sha256(
                    (ROOT / "docs" / "SUPPORTED_FORMATS.md").read_bytes()).hexdigest()
            else:
                assert rows[f"{prefix}PeareUiCud/{name}"] == hashlib.sha256(
                    source.read_bytes()).hexdigest()
        assert f"{prefix}PeareUiCud/delete.txt" not in rows
    finally:
        opened.close()

def _create_gnu_tar_v7_sample(tar_tool, archive, source):
    cygpath = Path(tar_tool).with_name("cygpath.exe")
    assert cygpath.is_file()

    tool_env = os.environ.copy()
    tool_env["PATH"] = str(Path(tar_tool).parent) + os.pathsep + tool_env.get("PATH", "")
    converted_archive = subprocess.run(
        [str(cygpath), "-u", str(archive)], capture_output=True, text=True,
        timeout=30, env=tool_env)
    converted_source_dir = subprocess.run(
        [str(cygpath), "-u", str(source.parent)], capture_output=True,
        text=True, timeout=30, env=tool_env)
    assert converted_archive.returncode == 0, (
        converted_archive.stdout + converted_archive.stderr)
    assert converted_source_dir.returncode == 0, (
        converted_source_dir.stdout + converted_source_dir.stderr)
    converted_paths = (converted_archive.stdout.strip(),
                       converted_source_dir.stdout.strip())
    creation = subprocess.run(
        [tar_tool, "--format=v7", "-cf", converted_paths[0], "-C",
         converted_paths[1], source.name], capture_output=True, text=True,
        timeout=60, env=tool_env)
    assert creation.returncode == 0, creation.stdout + creation.stderr
    assert archive.read_bytes()[257:263] == bytes(6)
    with tarfile.open(archive, "r:") as original:
        members = original.getmembers()
        assert len(members) == 1 and not members[0].isdir()
        original_name = members[0].name
        assert original.extractfile(members[0]).read() == source.read_bytes()
    return original_name


def test_tar_v7_large_add_latency_and_independent_readback(
        writer_lib, tmp_path):
    tar_tool = os.environ.get("PEARE_GNU_TAR", "")
    if not tar_tool or not Path(tar_tool).is_file():
        pytest.skip("Set PEARE_GNU_TAR to GNU tar for V7 sample creation")
    if not Path(tar_tool).with_name("cygpath.exe").is_file():
        pytest.skip("The GNU tar runtime directory must include cygpath.exe")

    source = tmp_path / "V7-SOURCE.TXT"
    source.write_bytes((ROOT / "README.md").read_bytes())
    sample = tmp_path / "tar-v7-large-add-sample.tar"
    _create_gnu_tar_v7_sample(tar_tool, sample, source)
    sample_hash = _sha256_file(sample)
    payload = tmp_path / "tar-v7-large-add-payload.bin"
    payload.write_bytes(os.urandom(16 * 1024 * 1024))
    payload_hash = _sha256_file(payload).hex()
    measurements = []

    for run in range(3):
        archive = tmp_path / f"tar-v7-large-add-{run}.tar"
        shutil.copyfile(sample, archive)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_files_begin(
                writer, str(archive).encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started

            started = time.perf_counter()
            status = writer_lib.peare_writer_add_file(
                writer, str(payload).encode(), b"LARGE-PAYLOAD.BIN")
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add_seconds = time.perf_counter() - started

            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            measurements.append((begin_seconds, add_seconds, finish_seconds))
        finally:
            writer_lib.peare_writer_destroy(writer)

        assert archive.read_bytes()[257:263] == bytes(6)
        with tarfile.open(archive, "r:") as updated:
            assert set(updated.getnames()) == {"V7-SOURCE.TXT", "LARGE-PAYLOAD.BIN"}
            with updated.extractfile("LARGE-PAYLOAD.BIN") as extracted:
                assert hashlib.sha256(extracted.read()).hexdigest() == payload_hash

    print("TAR V7 16 MiB add seconds:", measurements)
    assert max(row[1] for row in measurements) < 5.0
    assert max(row[2] for row in measurements) < 10.0
    assert _sha256_file(sample) == sample_hash

def test_tar_v7_cud_with_gnu_tar_sample(writer_lib, peare_lib, tmp_path):
    tar_tool = os.environ.get("PEARE_GNU_TAR", "")
    if not tar_tool or not Path(tar_tool).is_file():
        pytest.skip("Set PEARE_GNU_TAR to GNU tar for V7 sample creation")
    if not Path(tar_tool).with_name("cygpath.exe").is_file():
        pytest.skip("The GNU tar runtime directory must include cygpath.exe")
    archive = tmp_path / "gnu-tar-v7-sample.tar"
    source = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    original_name = _create_gnu_tar_v7_sample(tar_tool, archive, source)

    _assert_archive_duplicate_create_is_atomic(
        writer_lib, archive, original_name, tmp_path)
    created_source = ROOT / "README.md"
    replacement_source = ROOT / "AGENTS.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(created_source).encode(), b"CUD-CREATED.TXT") == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement_source).encode(), original_name.encode()) == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"CUD-CREATED.TXT", b"CUD-FINAL.TXT") == 0
        assert writer_lib.peare_writer_create_empty_file(
            writer, b"CUD-EMPTY.TXT") == 0
        assert writer_lib.peare_writer_create_directory(writer, b"CUD-DIR") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(created_source).encode(), b"CUD-DIR/CHILD.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"CUD-EMPTY.TXT", 0) == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"CUD-DIR", 1) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)

    with tarfile.open(archive, "r:") as updated:
        rows = {member.name: member for member in updated.getmembers()}
        assert set(rows) == {original_name, "CUD-FINAL.TXT"}
        assert updated.extractfile(rows[original_name]).read() == replacement_source.read_bytes()
        assert updated.extractfile(rows["CUD-FINAL.TXT"]).read() == created_source.read_bytes()
    opened = peare_lib.open_file(str(archive))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert rows[original_name] == hashlib.sha256(
            replacement_source.read_bytes()).hexdigest()
        assert rows["CUD-FINAL.TXT"] == hashlib.sha256(
            created_source.read_bytes()).hexdigest()
    finally:
        opened.close()

def test_tar_v7_create_from_zero_and_full_cud(writer_lib, peare_lib, tmp_path):
    tar_tool = shutil.which("tar.exe") or shutil.which("tar")
    if not tar_tool:
        pytest.skip("A system tar tool is required for independent TAR validation")

    assert writer_lib.peare_writer_create_variant_count(b"tar") == 2
    variant = _WriterVariant()
    assert writer_lib.peare_writer_get_create_variant(
        b"tar", 1, C.byref(variant)) == 0
    assert variant.id == b"v7"

    archive = tmp_path / "created-v7.tar"
    source = ROOT / "README.md"
    replacement = ROOT / "AGENTS.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"tar", b"v7", str(archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(writer, b"DOCS") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"DOCS/README.TXT") == 0
        assert writer_lib.peare_writer_create_empty_file(writer, b"EMPTY.TXT") == 0
        assert writer_lib.peare_writer_create_directory(writer, b"DEL-DIR") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"DEL-DIR/CHILD.TXT") == 0
        assert writer_lib.peare_writer_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    created_data = archive.read_bytes()
    assert created_data[257:263] == bytes(6)
    assert created_data[345:500] == bytes(155)
    listed = subprocess.run(
        [tar_tool, "-tf", str(archive)], capture_output=True, text=True,
        timeout=30)
    assert listed.returncode == 0, listed.stdout + listed.stderr
    with tarfile.open(archive, "r:") as created:
        created_members = {member.name: member for member in created.getmembers()}
        assert set(created_members) == {
            "DOCS", "DOCS/README.TXT", "EMPTY.TXT",
            "DEL-DIR", "DEL-DIR/CHILD.TXT"}
        assert created.extractfile(created_members["DOCS/README.TXT"]).read() == \
            source.read_bytes()

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, str(archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"ADDED.TXT") == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"DOCS/README.TXT") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"ADDED.TXT", b"RENAMED.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"EMPTY.TXT", 0) == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"DEL-DIR", 1) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    updated_data = archive.read_bytes()
    with tarfile.open(archive, "r:") as updated:
        for member in updated.getmembers():
            assert updated_data[member.offset + 257:member.offset + 263] == bytes(6)
    checked = subprocess.run(
        [tar_tool, "-tf", str(archive)], capture_output=True, text=True,
        timeout=30)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    with tarfile.open(archive, "r:") as updated:
        members = {member.name: member for member in updated.getmembers()}
        assert set(members) == {"DOCS", "DOCS/README.TXT", "RENAMED.TXT"}
        assert updated.extractfile(members["RENAMED.TXT"]).read() == source.read_bytes()
        assert updated.extractfile(members["DOCS/README.TXT"]).read() == \
            replacement.read_bytes()
    opened = peare_lib.open_file(str(archive))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert rows["RENAMED.TXT"] == hashlib.sha256(source.read_bytes()).hexdigest()
        assert rows["DOCS/README.TXT"] == hashlib.sha256(
            replacement.read_bytes()).hexdigest()
    finally:
        opened.close()

def test_tar_v7_directory_rename_limit_is_atomic(writer_lib, tmp_path):
    archive = tmp_path / "v7-rename-atomic.tar"
    source = ROOT / "README.md"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"tar", b"v7", str(archive).encode()) == 0
        assert writer_lib.peare_writer_create_directory(writer, b"DOCS") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"DOCS/README.TXT") == 0
        assert writer_lib.peare_writer_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0
        too_long = b"X" * 96
        status = writer_lib.peare_writer_rename_entry(
            writer, b"DOCS", too_long)
        assert status != 0
        assert "100" in writer_lib.peare_writer_last_error(
            writer).decode("utf-8") or "field" in writer_lib.peare_writer_last_error(
                writer).decode("utf-8").lower()
        assert writer_lib.peare_writer_rename_entry(
            writer, b"DOCS", b"SAFE") == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with tarfile.open(archive, "r:") as updated:
        members = {member.name for member in updated.getmembers()}
        assert members == {"SAFE", "SAFE/README.TXT"}
        assert updated.extractfile("SAFE/README.TXT").read() == source.read_bytes()

def test_tar_rename_latency_and_rewrite_io(writer_lib, tmp_path):
    tar_tool = shutil.which("tar.exe") or shutil.which("tar")
    if not tar_tool:
        pytest.skip("A system tar tool is required to create the independent sample")

    sample = tmp_path / "tar-latency-sample.tar"
    sources = (ROOT / "README.md", ROOT / "AGENTS.md")
    created = subprocess.run(
        [tar_tool, "-cf", str(sample), "-C", str(ROOT), "README.md",
         "AGENTS.md"], capture_output=True, text=True, timeout=60)
    assert created.returncode == 0, created.stdout + created.stderr
    sample_hash = _sha256_file(sample)
    measurements = []

    for run in range(3):
        archive = tmp_path / f"tar-latency-{run}.tar"
        shutil.copyfile(sample, archive)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_begin(
                writer, str(archive).encode())
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_rename_entry(
                writer, b"README.md", b"README-RENAMED.md")
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_finish(writer)
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, rename_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, rename_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        with tarfile.open(archive, "r:") as updated:
            members = {member.name: member for member in updated.getmembers()}
            assert set(members) == {"README-RENAMED.md", "AGENTS.md"}
            for name, source in zip(("README-RENAMED.md", "AGENTS.md"), sources):
                with updated.extractfile(members[name]) as extracted:
                    assert extracted.read() == source.read_bytes()

    print("TAR rename seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 2.0
    assert _sha256_file(sample) == sample_hash

def test_tar_large_add_latency_and_independent_readback(writer_lib, tmp_path):
    tar_tool = shutil.which("tar.exe") or shutil.which("tar")
    if not tar_tool:
        pytest.skip("A system tar tool is required to create the independent sample")

    sample = tmp_path / "tar-large-add-sample.tar"
    created = subprocess.run(
        [tar_tool, "-cf", str(sample), "-C", str(ROOT), "README.md"],
        capture_output=True, text=True, timeout=60)
    assert created.returncode == 0, created.stdout + created.stderr
    sample_hash = _sha256_file(sample)
    payload = tmp_path / "tar-large-add-payload.bin"
    payload.write_bytes(os.urandom(16 * 1024 * 1024))
    payload_hash = _sha256_file(payload)
    measurements = []

    for run in range(3):
        archive = tmp_path / f"tar-large-add-{run}.tar"
        shutil.copyfile(sample, archive)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_files_begin(
                writer, str(archive).encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_file(
                writer, str(payload).encode(), b"LARGE-PAYLOAD.BIN")
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add_seconds = time.perf_counter() - started
            add_io = tuple(after - before for after, before in
                           zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, add_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, add_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        with tarfile.open(archive, "r:") as updated:
            assert set(updated.getnames()) == {"README.md", "LARGE-PAYLOAD.BIN"}
            extracted = tmp_path / f"tar-large-readback-{run}.bin"
            with updated.extractfile("LARGE-PAYLOAD.BIN") as source:
                extracted.write_bytes(source.read())
            assert _sha256_file(extracted) == payload_hash

    print("TAR 16 MiB add seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 5.0
    assert max(row["seconds"][2] for row in measurements) < 10.0
    assert _sha256_file(sample) == sample_hash

@pytest.mark.parametrize("compression", ["store", "deflate"])
def test_zip_large_add_latency_by_compression(
        writer_lib, tmp_path, compression):
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        if candidate.is_file():
            seven_zip = str(candidate)
    if not seven_zip:
        pytest.skip("7-Zip is required to create the independent ZIP sample")

    sample = tmp_path / f"zip-large-add-{compression}-sample.zip"
    level = "0" if compression == "store" else "6"
    created = subprocess.run(
        [seven_zip, "a", "-tzip", f"-mx={level}", str(sample),
         "README.md", "AGENTS.md"], cwd=ROOT, capture_output=True,
        text=True, timeout=60)
    assert created.returncode == 0, created.stdout + created.stderr
    sample_hash = _sha256_file(sample)
    payload = tmp_path / "zip-large-add-payload.bin"
    payload.write_bytes(os.urandom(16 * 1024 * 1024))
    payload_hash = _sha256_file(payload).hex()
    source_hashes = {
        name: _sha256_file(ROOT / name).hex()
        for name in ("README.md", "AGENTS.md")
    }
    measurements = []

    for run in range(3):
        archive = tmp_path / f"zip-large-add-{compression}-{run}.zip"
        shutil.copyfile(sample, archive)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_files_begin(
                writer, str(archive).encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_file(
                writer, str(payload).encode(), b"LARGE-PAYLOAD.BIN")
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add_seconds = time.perf_counter() - started
            add_io = tuple(after - before for after, before in
                           zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, add_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, add_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        with zipfile.ZipFile(archive) as updated:
            assert updated.testzip() is None
            assert updated.read("LARGE-PAYLOAD.BIN") is not None
            assert hashlib.sha256(
                updated.read("LARGE-PAYLOAD.BIN")).hexdigest() == payload_hash
            for name, digest in source_hashes.items():
                assert hashlib.sha256(updated.read(name)).hexdigest() == digest
        checked = subprocess.run(
            [seven_zip, "t", str(archive)], capture_output=True, text=True,
            timeout=60)
        assert checked.returncode == 0, checked.stdout + checked.stderr

    print(f"ZIP {compression} 16 MiB add seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 5.0
    assert max(row["seconds"][2] for row in measurements) < 10.0
    assert _sha256_file(sample) == sample_hash

@pytest.mark.parametrize("archive_format", ["zip", "tar"])
def test_malformed_archive_update_is_rejected_atomically(
        writer_lib, tmp_path, archive_format):
    variable = f"PEARE_MALFORMED_{archive_format.upper()}_TEST_IMAGE"
    sample_value = os.environ.get(variable, "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip(f"Set {variable} to an external malformed archive sample")
    sample = Path(sample_value)
    archive = tmp_path / sample.name
    shutil.copyfile(sample, archive)
    if archive_format == "zip":
        with pytest.raises(zipfile.BadZipFile):
            zipfile.ZipFile(archive)
    else:
        with pytest.raises(tarfile.ReadError):
            tarfile.open(archive, "r:")
    corrupt_hash = _sha256_file(archive)
    sample_hash = _sha256_file(sample)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        result = writer_lib.peare_writer_update_begin(
            writer, str(archive).encode())
        assert result != 0, "Writer accepted a corrupted archive header"
        assert _sha256_file(archive) == corrupt_hash
    finally:
        writer_lib.peare_writer_abort(writer)
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(sample) == sample_hash

@pytest.mark.parametrize(("method", "expected_method"), [
    ("Copy", zipfile.ZIP_STORED),
    ("Deflate", zipfile.ZIP_DEFLATED),
])
def test_zip_add_files_and_cud_with_archive_created_by_7zip(
        writer_lib, peare_lib, tmp_path, method, expected_method):
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        if candidate.is_file():
            seven_zip = str(candidate)
    if not seven_zip:
        pytest.skip("7-Zip is required to create and verify the ZIP sample")

    archive = tmp_path / f"independent-{method.casefold()}.zip"
    original_sources = {name: ROOT / name for name in ("README.md", "AGENTS.md")}
    creation = subprocess.run(
        [seven_zip, "a", "-tzip", f"-mm={method}", str(archive),
         *original_sources.keys()], cwd=ROOT, capture_output=True, text=True,
        timeout=60)
    assert creation.returncode == 0, creation.stdout + creation.stderr
    with zipfile.ZipFile(archive) as original:
        assert original.testzip() is None
        assert {item.filename: item.compress_type for item in original.infolist()} == {
            name: expected_method for name in original_sources}
        original_hashes = {
            name: hashlib.sha256(original.read(name)).digest()
            for name in original_sources}

    _assert_archive_duplicate_create_is_atomic(
        writer_lib, archive, "README.md", tmp_path)
    added_source = ROOT / "THIRDPARTY.md"
    replacement_source = ROOT / "AGENTS.md"
    added_name = b"CUD-ADDED.TXT"
    inputs = (_WriterInputFile * 1)((str(added_source).encode(), added_name))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to(
            writer, str(archive).encode(), None, None, 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement_source).encode(), b"README.md") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, added_name, b"CUD-FINAL.TXT") == 0
        assert writer_lib.peare_writer_create_empty_file(
            writer, b"CUD-EMPTY.TXT") == 0
        assert writer_lib.peare_writer_create_directory(
            writer, b"CUD-KEEP") == 0
        assert writer_lib.peare_writer_create_directory(
            writer, b"CUD-REMOVE") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(added_source).encode(), b"CUD-REMOVE/CHILD.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"CUD-EMPTY.TXT", 0) == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"CUD-REMOVE", 1) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as updated:
        assert updated.testzip() is None
        members = {item.filename for item in updated.infolist()}
        assert members == {"README.md", "AGENTS.md", "CUD-FINAL.TXT", "CUD-KEEP/"}
        assert hashlib.sha256(updated.read("README.md")).digest() == \
            hashlib.sha256(replacement_source.read_bytes()).digest()
        assert hashlib.sha256(updated.read("AGENTS.md")).digest() == \
            original_hashes["AGENTS.md"]
        assert hashlib.sha256(updated.read("CUD-FINAL.TXT")).digest() == \
            hashlib.sha256(added_source.read_bytes()).digest()
    validation = subprocess.run(
        [seven_zip, "t", str(archive)], capture_output=True, text=True,
        timeout=60)
    assert validation.returncode == 0, validation.stdout + validation.stderr
    opener = peare_lib.open_file(str(archive))
    assert opener is not None
    try:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            replacement_source.read_bytes()).hexdigest()
        assert rows["AGENTS.md"]["sha256"] == hashlib.sha256(
            original_sources["AGENTS.md"].read_bytes()).hexdigest()
        assert rows["CUD-FINAL.TXT"]["sha256"] == hashlib.sha256(
            added_source.read_bytes()).hexdigest()
        assert "CUD-EMPTY.TXT" not in rows
        assert "CUD-REMOVE/CHILD.TXT" not in rows
    finally:
        opener.close()


def test_zip_deflate_create_works_with_both_archive_tools(
        writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    winrar = _winrar_executable()
    source = ROOT / "README.md"
    archive = tmp_path / "peare-deflate.zip"
    source_bytes = source.read_bytes()
    assert writer_lib.peare_writer_create_variant_count(b"zip") == 18
    variants = (_WriterVariant * 18)()
    assert all(writer_lib.peare_writer_get_create_variant(
        b"zip", index, C.byref(variants[index])) == 0 for index in range(18))
    assert [(item.id, item.display_name) for item in variants] == [
        (b"deflate", b"Deflate"), (b"bzip2", b"BZip2"),
        (b"store", b"Store"), (b"lzma", b"LZMA"),
        (b"xz", b"XZ (ZIPX)"), (b"zstd", b"Zstandard (ZIPX)"),
        (b"ppmd", b"PPMd (ZIPX)"), (b"shrink", b"Shrink"),
        (b"deflate64", b"Deflate64"), (b"mp3", b"MP3 (ZIPX)"),
        (b"wavpack", b"WavPack (ZIPX)"), (b"implode", b"Implode"),
        (b"dcl-implode", b"DCL Implode"),
        (b"reduce-1", b"Reduce 1"), (b"reduce-2", b"Reduce 2"),
        (b"reduce-3", b"Reduce 3"), (b"reduce-4", b"Reduce 4"),
        (b"jpeg", b"JPEG (ZIPX)")]

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"zip", b"deflate", str(archive).encode()) == 0
        assert writer_lib.peare_writer_zip_add_file(
            writer, str(source).encode(), b"README.md") == 0
        assert writer_lib.peare_writer_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.testzip() is None
        assert created.getinfo("README.md").compress_type == zipfile.ZIP_DEFLATED
        assert created.read("README.md") == source_bytes
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(source_bytes).hexdigest()

    batch_archive = tmp_path / "peare-deflate-batch.zip"
    options = _WriterCreateOptions(b"zip", b"deflate", None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(batch_archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with zipfile.ZipFile(batch_archive) as created:
        assert created.getinfo("README.md").compress_type == zipfile.ZIP_DEFLATED
        assert created.read("README.md") == source_bytes

    assert seven_zip
    tools = [(seven_zip, False)]
    if winrar:
        tools.append((winrar, True))
    for archive_path in (archive, batch_archive):
        for tool, is_winrar in tools:
            arguments = [tool, "t", "-ibck", str(archive_path)] \
                if is_winrar else [tool, "t", str(archive_path)]
            result = subprocess.run(arguments, capture_output=True, text=True,
                                    timeout=60)
            assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize(("variant", "method"), [
    (b"implode", 6), (b"dcl-implode", 10),
])
def test_zip_legacy_implode_writer_roundtrip(
        writer_lib, peare_lib, tmp_path, variant, method):
    source = ROOT / "README.md"
    source_bytes = source.read_bytes()
    archive = tmp_path / f"peare-method{method}.zip"
    options = _WriterCreateOptions(b"zip", variant, None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with zipfile.ZipFile(archive) as created:
        entry = created.getinfo("README.md")
        assert entry.compress_type == method
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()


@pytest.mark.parametrize(("variant", "method"), [
    (b"reduce-1", 2), (b"reduce-2", 3),
    (b"reduce-3", 4), (b"reduce-4", 5),
])
def test_zip_reduce_writer_roundtrip(writer_lib, peare_lib, tmp_path, variant, method):
    source = ROOT / "README.md"
    source_bytes = source.read_bytes()
    archive = tmp_path / f"peare-method{method}.zip"
    options = _WriterCreateOptions(b"zip", variant, None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with zipfile.ZipFile(archive) as created:
        entry = created.getinfo("README.md")
        assert entry.compress_type == method
        assert entry.extract_version == 10
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()
    hwzip = os.environ.get("PEARE_HWZIP_PATH")
    if hwzip:
        extracted = tmp_path / f"hwzip-method{method}"
        extracted.mkdir()
        result = subprocess.run([hwzip, "extract", str(archive)], cwd=extracted,
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        assert (extracted / "README.md").read_bytes() == source_bytes


@pytest.mark.parametrize(("factor", "method"), [(1, 2), (2, 3), (3, 4), (4, 5)])
def test_zip_reduce_reader_real_sample(peare_lib, factor, method):
    archive_value = os.environ.get(f"PEARE_ZIP_REDUCE_FACTOR{factor}_SAMPLE")
    if not archive_value:
        pytest.skip("Set PEARE_ZIP_REDUCE_FACTOR*_SAMPLE to external Reduce ZIP samples")
    archive = Path(archive_value)
    assert archive.is_file()
    with zipfile.ZipFile(archive) as external:
        members = [info for info in external.infolist() if not info.is_dir()]
    assert len(members) == 1
    info = members[0]
    assert info.compress_type == method
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows[info.filename]["sha256"] == hashlib.sha256(
            (ROOT / "README.md").read_bytes()).hexdigest()


@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize(("variant", "method"), [
    (b"reduce-1", 2), (b"reduce-2", 3),
    (b"reduce-3", 4), (b"reduce-4", 5),
])
def test_zip_reduce_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path,
        variant, method, view):
    source = ROOT / "README.md"
    archive = tmp_path / f"gui-reduce-{method}.zip"
    options = _WriterCreateOptions(b"zip", variant, None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(archive), view, "")
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    with peare_lib.open_file(report["image"]) as updated:
        rows = {row["path"]: row for row in updated.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source.read_bytes()).hexdigest()


@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize(("variant", "method"), [
    (b"implode", 6), (b"dcl-implode", 10),
])
def test_zip_legacy_implode_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path,
        variant, method, view):
    source = ROOT / "README.md"
    archive = tmp_path / f"gui-method{method}.zip"
    options = _WriterCreateOptions(b"zip", variant, None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(archive), view, "")
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    with peare_lib.open_file(report["image"]) as updated:
        rows = {row["path"]: row for row in updated.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source.read_bytes()).hexdigest()


def test_zip_bzip2_create_works_with_both_archive_tools(
        writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    winrar = _winrar_executable()
    source = ROOT / "README.md"
    archive = tmp_path / "peare-bzip2.zip"
    source_bytes = source.read_bytes()
    options = _WriterCreateOptions(b"zip", b"bzip2", None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.testzip() is None
        assert created.getinfo("README.md").compress_type == zipfile.ZIP_BZIP2
        assert created.read("README.md") == source_bytes
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()

    assert seven_zip
    tools = [(seven_zip, False)]
    if winrar:
        tools.append((winrar, True))
    for tool, is_winrar in tools:
        arguments = [tool, "t", "-ibck", str(archive)] \
            if is_winrar else [tool, "t", str(archive)]
        result = subprocess.run(arguments, capture_output=True, text=True,
                                timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr


def test_zip_lzma_create_works_with_both_archive_tools(
        writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    winrar = _winrar_executable()
    source = ROOT / "README.md"
    archive = tmp_path / "peare-lzma.zip"
    source_bytes = source.read_bytes()
    options = _WriterCreateOptions(b"zip", b"lzma", None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.testzip() is None
        assert created.getinfo("README.md").compress_type == zipfile.ZIP_LZMA
        assert created.read("README.md") == source_bytes
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()

    buffer_archive = tmp_path / "peare-lzma-buffer.zip"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"zip", b"lzma", str(buffer_archive).encode()) == 0
        assert writer_lib.peare_writer_zip_add_buffer(
            writer, b"README.md", source_bytes, len(source_bytes)) == 0
        assert writer_lib.peare_writer_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with zipfile.ZipFile(buffer_archive) as created:
        assert created.testzip() is None
        assert created.read("README.md") == source_bytes

    assert seven_zip
    tools = [(seven_zip, False)]
    if winrar:
        tools.append((winrar, True))
    for archive_path in (archive, buffer_archive):
        for tool, is_winrar in tools:
            arguments = [tool, "t", "-ibck", str(archive_path)] \
                if is_winrar else [tool, "t", str(archive_path)]
            result = subprocess.run(arguments, capture_output=True, text=True,
                                    timeout=60)
            assert result.returncode == 0, result.stdout + result.stderr


def test_zipx_xz_create_works_with_peare_and_archive_tools(
        writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    winrar = _winrar_executable()
    source = ROOT / "README.md"
    archive = tmp_path / "peare-xz.zipx"
    source_bytes = source.read_bytes()
    options = _WriterCreateOptions(b"zip", b"xz", None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.getinfo("README.md").compress_type == 95
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()

    tools = [(seven_zip, False)] if seven_zip else []
    if winrar:
        tools.append((winrar, True))
    if not tools:
        pytest.skip("No independent tool is installed for ZIPX XZ validation")
    unsupported = True
    for tool, is_winrar in tools:
        arguments = [tool, "t", "-ibck", str(archive)] \
            if is_winrar else [tool, "t", str(archive)]
        result = subprocess.run(arguments, capture_output=True, text=True,
                                timeout=60)
        if result.returncode == 0:
            unsupported = False
            break
        output = result.stdout + result.stderr
        if "Unsupported Method" not in output:
            assert result.returncode == 0, output
    if unsupported:
        pytest.skip("Installed archive tools do not support ZIPX XZ")


def test_zipx_zstandard_create_works_with_peare_and_archive_tools(
        writer_lib, peare_lib, tmp_path):
    winrar = _winrar_executable()
    source = ROOT / "README.md"
    archive = tmp_path / "peare-zstandard.zipx"
    source_bytes = source.read_bytes()
    options = _WriterCreateOptions(b"zip", b"zstd", None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.getinfo("README.md").compress_type == 93
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()

    buffer_archive = tmp_path / "peare-zstandard-buffer.zipx"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"zip", b"zstd", str(buffer_archive).encode()) == 0
        assert writer_lib.peare_writer_zip_add_buffer(
            writer, b"README.md", source_bytes, len(source_bytes)) == 0
        assert writer_lib.peare_writer_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with zipfile.ZipFile(buffer_archive) as created:
        assert created.getinfo("README.md").compress_type == 93
    with peare_lib.open_file(str(buffer_archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()
    seven_zip = _seven_zip_executable()
    tools = [(seven_zip, False)] if seven_zip else []
    if winrar:
        tools.append((winrar, True))
    if not tools:
        pytest.skip("No independent tool is installed for ZIPX Zstandard validation")
    for archive_path in (archive, buffer_archive):
        independently_validated = False
        for tool, is_winrar in tools:
            arguments = [tool, "t", "-ibck", str(archive_path)] \
                if is_winrar else [tool, "t", str(archive_path)]
            checked = subprocess.run(arguments, capture_output=True,
                                     text=True, timeout=60)
            if checked.returncode == 0:
                independently_validated = True
                break
            output = checked.stdout + checked.stderr
            if "Unsupported Method" not in output:
                assert checked.returncode == 0, output
        if not independently_validated:
            pytest.skip("Installed archive tools do not support ZIPX Zstandard")
        if winrar:
            extracted = tmp_path / f"winrar-extracted-{archive_path.stem}"
            extracted.mkdir()
            unpacked = subprocess.run(
                [winrar, "x", "-ibck", "-o+", str(archive_path),
                 str(extracted) + "\\"], capture_output=True, text=True,
                timeout=60)
            assert unpacked.returncode == 0, unpacked.stdout + unpacked.stderr
            assert (extracted / "README.md").read_bytes() == source_bytes


def test_zipx_ppmd_create_works_with_peare_and_winrar(
        writer_lib, peare_lib, tmp_path):
    winrar = _winrar_executable()
    source = ROOT / "README.md"
    source_bytes = source.read_bytes()
    archive = tmp_path / "peare-ppmd.zipx"
    options = _WriterCreateOptions(b"zip", b"ppmd", None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.getinfo("README.md").compress_type == 98
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()

    buffer_archive = tmp_path / "peare-ppmd-buffer.zipx"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"zip", b"ppmd", str(buffer_archive).encode()) == 0
        assert writer_lib.peare_writer_zip_add_buffer(
            writer, b"README.md", source_bytes, len(source_bytes)) == 0
        assert writer_lib.peare_writer_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with peare_lib.open_file(str(buffer_archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows["README.md"]["sha256"] == hashlib.sha256(
            source_bytes).hexdigest()

    seven_zip = _seven_zip_executable()
    tools = [(seven_zip, False)] if seven_zip else []
    if winrar:
        tools.append((winrar, True))
    if not tools:
        pytest.skip("No independent tool is installed for ZIPX PPMd validation")
    for archive_path in (archive, buffer_archive):
        independently_validated = False
        for tool, is_winrar in tools:
            arguments = [tool, "t", "-ibck", str(archive_path)] \
                if is_winrar else [tool, "t", str(archive_path)]
            checked = subprocess.run(arguments, capture_output=True,
                                     text=True, timeout=60)
            if checked.returncode == 0:
                independently_validated = True
                break
            output = checked.stdout + checked.stderr
            if "Unsupported Method" not in output:
                assert checked.returncode == 0, output
        if not independently_validated:
            pytest.skip("Installed archive tools do not support ZIPX PPMd")
        if winrar:
            extracted = tmp_path / f"winrar-{archive_path.stem}"
            extracted.mkdir()
            unpacked = subprocess.run(
                [winrar, "x", "-ibck", "-o+", str(archive_path),
                 str(extracted) + "\\"], capture_output=True, text=True,
                timeout=60)
            assert unpacked.returncode == 0, unpacked.stdout + unpacked.stderr
            assert (extracted / "README.md").read_bytes() == source_bytes


def test_zip_shrink_writer_create_roundtrip(writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    source = ROOT / "README.md"
    assert seven_zip
    source_bytes = source.read_bytes()
    archive = tmp_path / "peare-shrink.zip"
    options = _WriterCreateOptions(b"zip", b"shrink", None)
    input_file = _WriterInputFile(str(source).encode(), b"README.md")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.getinfo("README.md").compress_type == 1
    with peare_lib.open_file(str(archive)) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(
            opener._h, 0, 0, C.byref(resource)) == 0
        try:
            payload = po._Blob()
            assert opener._L.peare_resource_get_payload(
                resource, C.byref(payload)) == 0
            try:
                assert hashlib.sha256(payload.to_bytes()).digest() == \
                    hashlib.sha256(source_bytes).digest()
            finally:
                opener._L.peare_blob_free(C.byref(payload))
        finally:
            opener._L.peare_resource_destroy(resource)
    result = subprocess.run([seven_zip, "t", str(archive)],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr


def _zipx_method94_sample(peare_lib, tmp_path):
    archive = os.environ.get("PEARE_ZIPX_MP3_ARCHIVE", "")
    if not archive or not Path(archive).is_file():
        pytest.skip("Set PEARE_ZIPX_MP3_ARCHIVE to the WinZip method-94 sample")
    with zipfile.ZipFile(archive) as source:
        info = next(item for item in source.infolist() if item.compress_type == 94)
        name = info.filename
    raw_payload = _zip_raw_member_payload(archive, name)
    with peare_lib.open_file(archive) as opener:
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(
            opener._h, 0, 0, C.byref(resource)) == 0
        try:
            payload = po._Blob()
            assert opener._L.peare_resource_get_payload(
                resource, C.byref(payload)) == 0
            data = payload.to_bytes()
            opener._L.peare_blob_free(C.byref(payload))
        finally:
            opener._L.peare_resource_destroy(resource)
    sample = tmp_path / Path(name).name
    sample.write_bytes(data)
    return sample, name, raw_payload, data


def test_zipx_mp3_writer_matches_winzip_payload(writer_lib, peare_lib, tmp_path):
    sample, name, original_packed, source_bytes = _zipx_method94_sample(
        peare_lib, tmp_path)
    archive = tmp_path / "peare-method94.zip"
    options = _WriterCreateOptions(b"zip", b"mp3", None)
    input_file = _WriterInputFile(str(sample).encode(), name.encode())
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.getinfo(name).compress_type == 94
    assert _zip_raw_member_payload(archive, name) == original_packed
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows[name]["sha256"] == hashlib.sha256(source_bytes).hexdigest()


@pytest.mark.parametrize("view", ["tree", "details"])
def test_zipx_mp3_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view):
    sample, name, _, _ = _zipx_method94_sample(peare_lib, tmp_path)
    archive = tmp_path / "gui-method94.zip"
    options = _WriterCreateOptions(b"zip", b"mp3", None)
    input_file = _WriterInputFile(str(sample).encode(), name.encode())
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(archive), view, "")
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    with zipfile.ZipFile(report["image"]) as updated:
        assert updated.getinfo(name).compress_type == 94


def _zipx_method97_sample(peare_lib, tmp_path):
    archive = os.environ.get("PEARE_ZIPX_WAVPACK_ARCHIVE", "")
    if not archive or not Path(archive).is_file():
        pytest.skip("Set PEARE_ZIPX_WAVPACK_ARCHIVE to the WinZip method-97 sample")
    with peare_lib.open_file(archive) as opener:
        candidates = []
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = _HANDLE()
                assert opener._L.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)) == 0
                try:
                    context = po._Context()
                    assert opener._L.peare_resource_get_context(
                        resource, C.byref(context)) == 0
                    name = context.identifier_utf8.to_bytes().decode(
                        "utf-8", "replace")
                    size = int(context.data_size)
                    opener._L.peare_resource_context_free(C.byref(context))
                    if name.lower().endswith(".wav"):
                        candidates.append((size, folder, index, name))
                finally:
                    opener._L.peare_resource_destroy(resource)
        assert candidates
        _, folder, index, name = min(candidates)
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(
            opener._h, folder, index, C.byref(resource)) == 0
        try:
            payload = po._Blob()
            assert opener._L.peare_resource_get_payload(
                resource, C.byref(payload)) == 0
            data = payload.to_bytes()
            opener._L.peare_blob_free(C.byref(payload))
        finally:
            opener._L.peare_resource_destroy(resource)
    sample = tmp_path / Path(name).name
    sample.write_bytes(data)
    return sample, name, data


def test_zipx_wavpack_writer_roundtrip(writer_lib, peare_lib, tmp_path):
    sample, name, source_bytes = _zipx_method97_sample(peare_lib, tmp_path)
    archive = tmp_path / "peare-method97.zip"
    options = _WriterCreateOptions(b"zip", b"wavpack", None)
    input_file = _WriterInputFile(str(sample).encode(), name.encode())
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    with zipfile.ZipFile(archive) as created:
        assert created.getinfo(name).compress_type == 97
    with peare_lib.open_file(str(archive)) as opener:
        payload = None
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = _HANDLE()
                assert opener._L.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)) == 0
                try:
                    context = po._Context()
                    assert opener._L.peare_resource_get_context(
                        resource, C.byref(context)) == 0
                    resource_name = context.identifier_utf8.to_bytes().decode(
                        "utf-8", "replace")
                    opener._L.peare_resource_context_free(C.byref(context))
                    if resource_name != name:
                        continue
                    blob = po._Blob()
                    assert opener._L.peare_resource_get_payload(
                        resource, C.byref(blob)) == 0
                    payload = blob.to_bytes()
                    opener._L.peare_blob_free(C.byref(blob))
                finally:
                    opener._L.peare_resource_destroy(resource)
                if payload is not None:
                    break
            if payload is not None:
                break
        assert payload == source_bytes


def test_zipx_jpeg_method96_writer_roundtrip(writer_lib, peare_lib, tmp_path):
    source_path = os.environ.get("PEARE_ZIPX_JPEG_SOURCE", "")
    if not source_path or not Path(source_path).is_file():
        pytest.skip("Set PEARE_ZIPX_JPEG_SOURCE to an external JPEG sample")
    sample = Path(source_path)
    name = sample.name
    source_bytes = sample.read_bytes()
    archive = tmp_path / "peare-method96.zipx"
    options = _WriterCreateOptions(b"zip", b"jpeg", None)
    input_file = _WriterInputFile(str(sample).encode(), name.encode())
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.getinfo(name).compress_type == 96
    with peare_lib.open_file(str(archive)) as opener:
        found = False
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = _HANDLE()
                assert opener._L.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)) == 0
                try:
                    context = po._Context()
                    assert opener._L.peare_resource_get_context(
                        resource, C.byref(context)) == 0
                    resource_name = context.identifier_utf8.to_bytes().decode(
                        "utf-8", "replace")
                    opener._L.peare_resource_context_free(C.byref(context))
                    if resource_name != name:
                        continue
                    blob = po._Blob()
                    status = opener._L.peare_resource_get_payload(
                        resource, C.byref(blob))
                    assert status == 0, f"Reader payload status: {status}"
                    assert blob.to_bytes() == source_bytes
                    opener._L.peare_blob_free(C.byref(blob))
                    found = True
                finally:
                    opener._L.peare_resource_destroy(resource)
                if found:
                    break
            if found:
                break
        assert found


def test_zipx_jpeg_method96_full_cud(writer_lib, peare_lib, tmp_path):
    archive_value = os.environ.get("PEARE_ZIPX_JPEG_ARCHIVE", "")
    source_value = os.environ.get("PEARE_ZIPX_JPEG_SOURCE", "")
    if not archive_value or not Path(archive_value).is_file():
        pytest.skip("Set PEARE_ZIPX_JPEG_ARCHIVE to an authentic WinZip sample")
    if not source_value or not Path(source_value).is_file():
        pytest.skip("Set PEARE_ZIPX_JPEG_SOURCE to its original JPEG payload")
    sample = Path(archive_value)
    source = Path(source_value)
    archive = tmp_path / "method96-cud.zipx"
    shutil.copy2(sample, archive)
    with zipfile.ZipFile(archive) as created:
        assert created.getinfo(source.name).compress_type == 96

    extra = ROOT / "README.md"
    inputs = (_WriterInputFile * 1)(
        _WriterInputFile(str(extra).encode(), b"added.txt"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to(
            writer, str(archive).encode(), None, None, 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, source.name.encode(), b"renamed.jpg") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"added.txt", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert set(rows) == {"renamed.jpg"}
        assert rows["renamed.jpg"]["sha256"] == hashlib.sha256(
            source.read_bytes()).hexdigest()


@pytest.mark.parametrize("view", ["tree", "details"])
def test_zipx_jpeg_method96_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view):
    archive_value = os.environ.get("PEARE_ZIPX_JPEG_ARCHIVE", "")
    if not archive_value or not Path(archive_value).is_file():
        pytest.skip("Set PEARE_ZIPX_JPEG_ARCHIVE to an authentic WinZip sample")
    archive = tmp_path / "gui-method96.zipx"
    shutil.copy2(archive_value, archive)
    with zipfile.ZipFile(archive) as created:
        assert all(item.compress_type == 96 for item in created.infolist())
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(archive), view, "")
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    with peare_lib.open_file(report["image"]) as updated:
        rows = {row["path"]: row for row in updated.normalized_tree()}
        assert len(rows) >= 2


def test_gzip_writer_multi_member_roundtrip_and_7zip(
        writer_lib, peare_lib, tmp_path):
    seven_zip = _seven_zip_executable()
    sources = [ROOT / "README.md", ROOT / "docs" / "SUPPORTED_FORMATS.md"]
    source_bytes = [path.read_bytes() for path in sources]
    member_names = ["Peare-README-é.md", sources[1].name]
    formats = []
    for index in range(writer_lib.peare_writer_create_format_count()):
        item = _WriterFormat()
        assert writer_lib.peare_writer_get_create_format(index, C.byref(item)) == 0
        formats.append(item.id.decode())
    assert "gzip" in formats
    assert writer_lib.peare_writer_create_variant_count(b"gzip") == 1
    variant = _WriterVariant()
    assert writer_lib.peare_writer_get_create_variant(
        b"gzip", 0, C.byref(variant)) == 0
    assert (variant.id, variant.display_name) == (b"deflate", b"Deflate")
    archive = tmp_path / "peare-gzip.gz"
    options = _WriterCreateOptions(b"gzip", b"deflate", None)
    inputs = (_WriterInputFile * len(sources))(*[
        _WriterInputFile(str(path).encode(), name.encode())
        for path, name in zip(sources, member_names)])
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            inputs, len(sources))
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    expected = b"".join(source_bytes)
    with gzip.open(archive, "rb") as decoded:
        assert decoded.read() == expected
    if seven_zip:
        external_check = subprocess.run(
            [seven_zip, "t", str(archive)], capture_output=True,
            text=True, timeout=60, check=False)
        assert external_check.returncode == 0, \
            external_check.stdout + external_check.stderr
    with peare_lib.open_file(str(archive)) as opener:
        rows = {row["path"]: row for row in opener.normalized_tree()}
        assert rows[member_names[0]]["sha256"] == hashlib.sha256(expected).hexdigest()


def test_writer_create_format_through_gui_dialog(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    sample = ROOT / "tests" / "formats" / "tar" / "fixtures" / "docs.tar"
    assert sample.is_file()
    format_sources = [
        ("zip", "README.md"), ("gzip", "README.md"),
        ("bzip2", "README.md"), ("zstd", "README.md"),
        ("xz", "README.md"), ("lzma", "README.md"),
        ("tar", "README.md"), ("tar-gzip", "README.md"),
        ("tar-bzip2", "README.md"), ("tar-zstd", "README.md"),
        ("tar-unix-compress", "README.md"), ("tar-xz", "README.md"),
        ("tar-lzma", "README.md"), ("cpio", "README.md"),
        ("ar", "README.md"), ("wad", "LICENSE"),
        ("unix-compress", "README.md"), ("lzip", "README.md"),
        ("uuencode", "README.md"),
        ("ihex", "tests/formats/ihex/fixtures/README-low-address.hex"),
        ("deb", "README.md"), ("rpm", "README.md"),
        ("xar", "README.md"), ("iso", "README.md"),
        ("fat", "README.md"), ("floppy", "README.md"),
        ("raw-disk", "README.md"), ("vhd", "README.md"),
        ("vhdx", "README.md"), ("vmdk", "README.md"),
    ]
    format_info = _WriterFormat()
    formats = {}
    for index in range(writer_lib.peare_writer_create_format_count()):
        assert writer_lib.peare_writer_get_create_format(
            index, C.byref(format_info)) == 0
        formats[format_info.id.decode()] = format_info.extension.decode()

    operations = []
    expected = {}
    for format_index, (format_id, source_relative) in enumerate(format_sources):
        assert format_id in formats
        source = ROOT / source_relative
        assert source.is_file()
        variants = []
        format_id_bytes = format_id.encode()
        for index in range(writer_lib.peare_writer_create_variant_count(
                format_id_bytes)):
            variant = _WriterVariant()
            assert writer_lib.peare_writer_get_create_variant(
                format_id_bytes, index, C.byref(variant)) == 0
            variants.append(variant.id.decode())
        assert variants

        filesystems = []
        for index in range(writer_lib.peare_writer_create_filesystem_count(
                format_id_bytes)):
            filesystem = _WriterVariant()
            assert writer_lib.peare_writer_get_create_filesystem(
                format_id_bytes, index, C.byref(filesystem)) == 0
            filesystems.append(filesystem.id.decode())
        if not filesystems:
            filesystems = [None]

        for variant_index, variant_id in enumerate(variants):
            for filesystem_index, filesystem_id in enumerate(filesystems):
                suffix = f"{format_index}-{variant_index}-{filesystem_index}"
                destination = tmp_path / f"created-{suffix}.{formats[format_id]}"
                operation_source = source
                operation = {
                    "type": "create_container",
                    "format": format_id,
                    "variant": variant_id,
                    "destination": str(destination),
                    "sources": [str(source)],
                    "test_id": f"{format_id}/{variant_id}/{filesystem_id or 'default'}",
                    "cleanup_created_container": True,
                }
                if filesystem_id:
                    operation["filesystem"] = filesystem_id
                if format_id == "zip" and variant_id in {"jpeg", "wavpack"}:
                    sample_root = Path.home() / "Documents" / "inerente a" / "Peare" / "zipx-validation"
                    relative_sample = (
                        Path("method96") / "peare-logo-444.jpg"
                        if variant_id == "jpeg" else
                        Path("Abbrevia_WavPack") / "source" / "default.wav")
                    operation_source = sample_root / relative_sample
                    if not operation_source.is_file():
                        pytest.skip(f"Missing real ZIPX {variant_id} source sample")
                    operation["sources"] = [str(operation_source)]
                if format_id == "zip" and variant_id == "mp3":
                    mp3_source, _, _, _ = _zipx_method94_sample(peare_lib, tmp_path)
                    operation["sources"] = [str(mp3_source)]
                    operation_source = mp3_source
                expected[destination] = (
                    None if format_id == "ihex"
                    else hashlib.sha256(operation_source.read_bytes()).hexdigest())
                operations.append(operation)

    assert operations
    operations[-1]["cleanup_created_container"] = False
    scenario_path = tmp_path / "gui-create-all-variants.json"
    scenario_path.write_text(json.dumps({"view": "tree", "operations": operations}),
                             encoding="utf-8")
    output_dir = tmp_path / "gui-create-all-variants"
    result = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
         "--exe", str(peare_gui_path), "--image", str(sample),
         "--scenario", str(scenario_path), "--output-dir", str(output_dir),
         "--timeout", "900"],
        capture_output=True, text=True, timeout=920)
    assert result.returncode == 0, result.stdout + result.stderr
    report = json.loads((output_dir / "writer-test-report.json").read_text(
        encoding="utf-8"))
    assert report["stage"] == "complete"
    assert len(report["operations"]) == len(operations)
    assert all(item["passed"] for item in report["operations"])
    assert Path(report["image"]) == list(expected)[-1].resolve()
    for destination in list(expected)[:-1]:
        assert not destination.exists(), destination
    destination = list(expected)[-1]
    assert destination.is_file()
    with peare_lib.open_file(str(destination)) as opened:
        assert opened is not None
        rows = opened.normalized_tree()
        assert rows
        expected_hash = expected[destination]
        if expected_hash:
            assert any(row.get("sha256") == expected_hash for row in rows)
    if destination.suffix.casefold() == ".vmdk":
        descriptor = destination.read_text(encoding="utf-8")
        for line in descriptor.splitlines():
            match = re.match(
                r'^(?:RW|RDONLY|NOACCESS)\s+\d+\s+\S+\s+"([^"]+)"',
                line.strip())
            if not match:
                continue
            relative = Path(match.group(1))
            assert not relative.is_absolute()
            extent = (destination.parent / relative).resolve()
            assert extent.is_relative_to(destination.parent.resolve())
            if extent.exists():
                extent.unlink()
    destination.unlink()


def test_bzip2_writer_levels_concatenated_streams_and_7zip(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    seven_zip = _seven_zip_executable()
    sources = [ROOT / "README.md"]
    expected = sources[0].read_bytes()
    expected_hash = hashlib.sha256(expected).hexdigest()
    variants = []
    for index in range(writer_lib.peare_writer_create_variant_count(b"bzip2")):
        item = _WriterVariant()
        assert writer_lib.peare_writer_get_create_variant(
            b"bzip2", index, C.byref(item)) == 0
        variants.append(item.id.decode())
    assert variants == [f"level-{level}" for level in range(1, 10)]

    inputs = (_WriterInputFile * len(sources))(*[
        _WriterInputFile(str(path).encode(), path.name.encode())
        for path in sources])
    gui_archive = None
    for variant in variants:
        archive = tmp_path / f"peare-{variant}.bz2"
        options = _WriterCreateOptions(b"bzip2", variant.encode(), None)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            status = writer_lib.peare_writer_add_files_to_ex(
                writer, str(archive).encode(), C.byref(options), 0,
                inputs, len(sources))
            assert status == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        if variant == "level-9":
            gui_archive = archive

        assert bz2.decompress(archive.read_bytes()) == expected
        if seven_zip:
            external_check = subprocess.run(
                [seven_zip, "t", str(archive)], capture_output=True,
                text=True, timeout=60, check=False)
            assert external_check.returncode == 0, \
                external_check.stdout + external_check.stderr
        with peare_lib.open_file(str(archive)) as opener:
            rows = opener.normalized_tree()
            assert len(rows) == 1
            assert rows[0]["sha256"] == expected_hash

    gui_output = tmp_path / "bzip2-gui-preview"
    gui_check = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
         "--exe", str(peare_gui_path), "--case", "bzip2-writer-level9",
         str(gui_archive), "peare-level-9", "--output-dir", str(gui_output)],
        capture_output=True, text=True, timeout=120, check=False)
    assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr


def test_zstd_writer_levels_concatenated_frames_and_7zip(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    seven_zip = _seven_zip_executable()
    zstd_cli = os.environ.get("PEARE_ZSTD_TEST_EXECUTABLE") or \
        shutil.which("zstd.exe") or shutil.which("zstd")
    sources = [ROOT / "README.md", ROOT / "docs" / "SUPPORTED_FORMATS.md"]
    expected = b"".join(path.read_bytes() for path in sources)
    expected_hash = hashlib.sha256(expected).hexdigest()
    variants = []
    for index in range(writer_lib.peare_writer_create_variant_count(b"zstd")):
        item = _WriterVariant()
        assert writer_lib.peare_writer_get_create_variant(
            b"zstd", index, C.byref(item)) == 0
        variants.append(item.id.decode())
    assert variants == [f"fast-{level}" for level in range(5, 0, -1)] + [
        f"level-{level}" for level in range(1, 23)]

    inputs = (_WriterInputFile * len(sources))(*[
        _WriterInputFile(str(path).encode(), path.name.encode())
        for path in sources])
    gui_archive = None
    for variant in variants:
        archive = tmp_path / f"peare-{variant}.zst"
        options = _WriterCreateOptions(b"zstd", variant.encode(), None)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            status = writer_lib.peare_writer_add_files_to_ex(
                writer, str(archive).encode(), C.byref(options), 0,
                inputs, len(sources))
            assert status == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        if variant == "level-3":
            gui_archive = archive

        if zstd_cli:
            external_extract = subprocess.run(
                [zstd_cli, "-q", "-d", "-c", str(archive)],
                capture_output=True, timeout=60, check=False)
            assert external_extract.returncode == 0, external_extract.stderr.decode(
                "utf-8", "replace")
            assert external_extract.stdout == expected
        elif seven_zip:
            version = subprocess.run([seven_zip], capture_output=True,
                                     text=True, timeout=30, check=False)
            version_text = version.stdout + version.stderr
            version_match = re.search(r"7-Zip\s+(\d+)\.(\d+)", version_text)
            if version_match and tuple(map(int, version_match.groups())) >= (24, 1):
                external_extract = subprocess.run(
                    [seven_zip, "x", "-so", str(archive)], capture_output=True,
                    timeout=60, check=False)
                assert external_extract.returncode == 0, external_extract.stderr.decode(
                    "utf-8", "replace")
                assert external_extract.stdout == expected
        with peare_lib.open_file(str(archive)) as opener:
            rows = opener.normalized_tree()
            assert len(rows) == 1
            assert rows[0]["sha256"] == expected_hash

    gui_output = tmp_path / "zstd-gui-preview"
    gui_check = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
         "--exe", str(peare_gui_path), "--case", "zstd-writer-level3",
         str(gui_archive), "peare-level-3", "--output-dir", str(gui_output)],
        capture_output=True, text=True, timeout=120, check=False)
    assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr


def test_xz_writer_integrity_checks_concatenated_streams_and_7zip(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    seven_zip = _seven_zip_executable()
    sources = [ROOT / "README.md", ROOT / "docs" / "SUPPORTED_FORMATS.md"]
    expected = b"".join(path.read_bytes() for path in sources)
    expected_hash = hashlib.sha256(expected).hexdigest()
    variants = []
    for index in range(writer_lib.peare_writer_create_variant_count(b"xz")):
        item = _WriterVariant()
        assert writer_lib.peare_writer_get_create_variant(
            b"xz", index, C.byref(item)) == 0
        variants.append(item.id.decode())
    assert variants == ["none", "crc32", "crc64", "sha256"]

    inputs = (_WriterInputFile * len(sources))(*[
        _WriterInputFile(str(path).encode(), path.name.encode())
        for path in sources])
    gui_archive = None
    for variant in variants:
        archive = tmp_path / f"peare-{variant}.xz"
        options = _WriterCreateOptions(b"xz", variant.encode(), None)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            status = writer_lib.peare_writer_add_files_to_ex(
                writer, str(archive).encode(), C.byref(options), 0,
                inputs, len(sources))
            assert status == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        if variant == "crc64":
            gui_archive = archive

        assert lzma.decompress(archive.read_bytes()) == expected
        if seven_zip:
            external_extract = subprocess.run(
                [seven_zip, "x", "-so", str(archive)], capture_output=True,
                timeout=60, check=False)
            assert external_extract.returncode == 0, external_extract.stderr.decode(
                "utf-8", "replace")
            assert external_extract.stdout == expected
        with peare_lib.open_file(str(archive)) as opener:
            rows = opener.normalized_tree()
            assert len(rows) == 1
            assert rows[0]["sha256"] == expected_hash

    gui_output = tmp_path / "xz-gui-preview"
    gui_check = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
         "--exe", str(peare_gui_path), "--case", "xz-writer-crc64",
         str(gui_archive), "peare-crc64", "--output-dir", str(gui_output)],
        capture_output=True, text=True, timeout=120, check=False)
    assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr


def test_lzma_alone_writer_levels_single_stream_and_7zip(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    seven_zip = _seven_zip_executable()
    sources = [ROOT / "README.md"]
    expected = sources[0].read_bytes()
    expected_hash = hashlib.sha256(expected).hexdigest()
    variants = []
    for index in range(writer_lib.peare_writer_create_variant_count(b"lzma")):
        item = _WriterVariant()
        assert writer_lib.peare_writer_get_create_variant(
            b"lzma", index, C.byref(item)) == 0
        variants.append(item.id.decode())
    assert variants == [f"level-{level}" for level in range(1, 10)]

    inputs = (_WriterInputFile * len(sources))(*[
        _WriterInputFile(str(path).encode(), path.name.encode())
        for path in sources])
    gui_archive = None
    for variant in variants:
        archive = tmp_path / f"peare-{variant}.lzma"
        options = _WriterCreateOptions(b"lzma", variant.encode(), None)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            status = writer_lib.peare_writer_add_files_to_ex(
                writer, str(archive).encode(), C.byref(options), 0,
                inputs, len(sources))
            assert status == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        if variant == "level-5":
            gui_archive = archive

        assert lzma.decompress(archive.read_bytes(), format=lzma.FORMAT_ALONE) == expected
        if seven_zip:
            external_extract = subprocess.run(
                [seven_zip, "x", "-so", str(archive)], capture_output=True,
                timeout=60, check=False)
            assert external_extract.returncode == 0, external_extract.stderr.decode(
                "utf-8", "replace")
            assert external_extract.stdout == expected
        with peare_lib.open_file(str(archive)) as opener:
            rows = opener.normalized_tree()
            assert len(rows) == 1
            assert rows[0]["sha256"] == expected_hash

    gui_output = tmp_path / "lzma-gui-preview"
    gui_check = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
         "--exe", str(peare_gui_path), "--case", "lzma-writer-level5",
         str(gui_archive), "peare-level-5", "--output-dir", str(gui_output)],
        capture_output=True, text=True, timeout=120, check=False)
    assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr


def test_unix_compress_writer_modes_widths_and_7zip(
        writer_lib, peare_lib, peare_gui_path, tmp_path):
    seven_zip = _seven_zip_executable()
    source = ROOT / "src" / "writer" / "third_party" / "zstd" / "compress" / "zstd_compress.c"
    expected = source.read_bytes()
    expected_hash = hashlib.sha256(expected).hexdigest()
    variants = []
    for index in range(writer_lib.peare_writer_create_variant_count(b"unix-compress")):
        item = _WriterVariant()
        assert writer_lib.peare_writer_get_create_variant(
            b"unix-compress", index, C.byref(item)) == 0
        variants.append(item.id.decode())
    assert variants == [
        *(f"block-{width}" for width in range(9, 17)),
        *(f"legacy-{width}" for width in range(9, 17))]

    input_file = _WriterInputFile(str(source).encode(), source.name.encode())
    gui_stream = None
    for variant in variants:
        stream_path = tmp_path / f"peare-{variant}.Z"
        options = _WriterCreateOptions(b"unix-compress", variant.encode(), None)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            status = writer_lib.peare_writer_add_files_to_ex(
                writer, str(stream_path).encode(), C.byref(options), 0,
                C.byref(input_file), 1)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
        if variant == "block-16":
            gui_stream = stream_path

        encoded = stream_path.read_bytes()
        bits = int(variant.rsplit("-", 1)[1])
        block_mode = variant.startswith("block-")
        assert encoded[:3] == bytes((0x1f, 0x9d, bits | (0x80 if block_mode else 0)))
        with peare_lib.open_file(str(stream_path)) as opener:
            rows = opener.normalized_tree()
            assert len(rows) == 1
            assert rows[0]["sha256"] == expected_hash
        if seven_zip and not (
                variant.endswith("-9") and
                _seven_zip_version(seven_zip) == (26, 5)):
            # 7-Zip 26.05 also rejects ncompress-produced 9-bit streams.
            external_extract = subprocess.run(
                [seven_zip, "x", "-so", str(stream_path)], capture_output=True,
                timeout=60, check=False)
            assert external_extract.returncode == 0, external_extract.stdout + external_extract.stderr
            assert external_extract.stdout == expected

    gui_output = tmp_path / "unix-compress-gui-preview"
    gui_check = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
         "--exe", str(peare_gui_path), "--case", "unix-compress-writer-block16",
         str(gui_stream), "peare-block-16", "--output-dir", str(gui_output)],
        capture_output=True, text=True, timeout=120, check=False)
    assert gui_check.returncode == 0, gui_check.stdout + gui_check.stderr


@pytest.mark.parametrize("view", ["tree", "details"])
def test_zipx_wavpack_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view):
    sample, name, _ = _zipx_method97_sample(peare_lib, tmp_path)
    archive = tmp_path / "gui-method97.zip"
    options = _WriterCreateOptions(b"zip", b"wavpack", None)
    input_file = _WriterInputFile(str(sample).encode(), name.encode())
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(archive), view, "")
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    with zipfile.ZipFile(report["image"]) as updated:
        assert updated.getinfo(name).compress_type == 97


def test_zip_rename_latency_preserves_untouched_member_data(
        writer_lib, tmp_path):
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = (Path(os.environ.get("ProgramFiles", "C:/Program Files")) /
                     "7-Zip" / "7z.exe")
        if candidate.is_file():
            seven_zip = str(candidate)
    if not seven_zip:
        pytest.skip("7-Zip is required to create the independent ZIP sample")

    sample = tmp_path / "zip-latency-sample.zip"
    sources = (ROOT / "README.md", ROOT / "AGENTS.md")
    created = subprocess.run(
        [seven_zip, "a", "-tzip", "-mm=Deflate", str(sample),
         *(path.name for path in sources)], cwd=ROOT, capture_output=True,
        text=True, timeout=60)
    assert created.returncode == 0, created.stdout + created.stderr
    sample_hash = _sha256_file(sample)
    untouched_before = _zip_raw_member_payload(sample, "AGENTS.md")
    measurements = []

    for run in range(3):
        archive = tmp_path / f"zip-latency-{run}.zip"
        shutil.copyfile(sample, archive)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_begin(
                writer, str(archive).encode())
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_rename_entry(
                writer, b"README.md", b"README-RENAMED.md")
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_finish(writer)
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, rename_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, rename_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        with zipfile.ZipFile(archive) as updated:
            assert updated.testzip() is None
            assert updated.read("README-RENAMED.md") == sources[0].read_bytes()
            assert updated.read("AGENTS.md") == sources[1].read_bytes()
        assert _zip_raw_member_payload(archive, "AGENTS.md") == untouched_before

    print("ZIP Deflate rename seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 2.0
    assert _sha256_file(sample) == sample_hash

def test_zip64_large_rename_latency_preserves_untouched_member_data(
        writer_lib, tmp_path):
    sample = _zip64_sample_from_env()
    if not sample or not sample.is_file():
        pytest.skip("Set PEARE_ZIP64_TEST_IMAGE to an external-tool ZIP64 sample")
    with zipfile.ZipFile(sample) as source_archive:
        assert len(source_archive.infolist()) == 65536
        assert source_archive.testzip() is None
        renamed_payload = source_archive.read("FILE00001.TXT")
        untouched_payload = source_archive.read("FILE65536.TXT")
    sample_hash = _sha256_file(sample)
    untouched_name = "FILE65536.TXT"
    untouched_before = _zip_raw_member_payload(sample, untouched_name)
    measurements = []

    for run in range(3):
        archive = tmp_path / f"zip64-latency-{run}.zip"
        shutil.copyfile(sample, archive)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_begin(
                writer, str(archive).encode())
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_rename_entry(
                writer, b"FILE00001.TXT", b"RENAMED.TXT")
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_finish(writer)
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, rename_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, rename_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        with zipfile.ZipFile(archive) as updated:
            assert len(updated.infolist()) == 65536
            assert updated.read("RENAMED.TXT") == renamed_payload
            assert updated.read(untouched_name) == untouched_payload
        assert _zip_raw_member_payload(archive, untouched_name) == untouched_before

    print("ZIP64 65,536-entry rename seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 2.0
    assert max(row["io_bytes_read_written"][0][0] for row in measurements) < \
        sample.stat().st_size * 4
    assert _sha256_file(sample) == sample_hash

def test_zip64_large_add_latency_and_readback(
        writer_lib, peare_lib, tmp_path):
    sample = _zip64_sample_from_env()
    if not sample:
        sample = ROOT / "tmp" / "writer-independent-samples" / "zip64-count.zip"
    if not sample.is_file():
        pytest.skip("Set PEARE_ZIP64_TEST_IMAGE to the external-tool ZIP64 sample")
    if os.name != "nt":
        pytest.skip("Process I/O counters are available only on Windows")

    with zipfile.ZipFile(sample) as source_archive:
        assert len(source_archive.infolist()) == 65536
        retained_payload = source_archive.read("FILE65536.TXT")
    sample_hash = _sha256_file(sample)
    payload = tmp_path / "zip64-large-add-payload.bin"
    payload.write_bytes(os.urandom(16 * 1024 * 1024))
    payload_hash = _sha256_file(payload).hex()
    measurements = []

    for run in range(3):
        archive = tmp_path / f"zip64-large-add-{run}.zip"
        shutil.copyfile(sample, archive)
        _assert_archive_duplicate_create_is_atomic(
            writer_lib, archive, "FILE65536.TXT", tmp_path)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_begin(
                writer, str(archive).encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_create_file(
                writer, str(payload).encode(),
                f"PEARE-LARGE-{run}.BIN".encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add = time.perf_counter() - started
            add_io = tuple(after - before for after, before in
                           zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin, add, finish),
                "io_bytes_read_written": (begin_io, add_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        with zipfile.ZipFile(archive) as updated:
            assert len(updated.infolist()) == 65537
            assert updated.read(f"PEARE-LARGE-{run}.BIN") == payload.read_bytes()
            assert updated.read("FILE65536.TXT") == retained_payload

        opened = peare_lib.open_file(str(archive))
        assert opened is not None
        try:
            row = next(item for item in opened.normalized_tree()
                       if item["path"].endswith(f"PEARE-LARGE-{run}.BIN"))
            assert row["sha256"] == payload_hash
        finally:
            opened.close()

    print("ZIP64 16 MiB add seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 5.0
    assert _sha256_file(sample) == sample_hash

@pytest.mark.parametrize("suffix", (
    "zip", "jar", "war", "ear", "aar", "apks", "aab", "epub",
    "docx", "xlsx", "pptx", "odt", "ods", "odp", "ipa", "ipsw", "xpi",
    "nupkg", "snupkg", "whl", "vsix", "cbz", "kmz"))
def test_zip_create_through_generic_add_files_to(writer_lib, tmp_path, suffix):
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        if candidate.is_file():
            seven_zip = str(candidate)
    if not seven_zip:
        pytest.skip("7-Zip is required to verify the ZIP output")

    archive = tmp_path / f"peare-created.{suffix}"
    source = ROOT / "README.md"
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"README.md"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to(
            writer, str(archive).encode(), b"zip", b"store", 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as output:
        assert output.testzip() is None
        assert output.namelist() == ["README.md"]
        assert output.getinfo("README.md").compress_type == zipfile.ZIP_STORED
        assert output.read("README.md") == source.read_bytes()
    validation = subprocess.run(
        [seven_zip, "t", str(archive)], capture_output=True, text=True,
        timeout=60)
    assert validation.returncode == 0, validation.stdout + validation.stderr

@pytest.mark.parametrize("suffix", (
    "jar", "war", "ear", "aar", "apk", "apks", "aab", "epub", "docx",
    "xlsx", "pptx", "odt", "ods", "odp", "ipa", "ipsw", "xpi", "nupkg",
    "snupkg", "whl", "vsix", "cbz", "kmz"))
def test_zip_writer_uses_container_records_not_filename_suffix(
        writer_lib, peare_lib, tmp_path, suffix):
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        seven_zip = str(candidate) if candidate.is_file() else None
    if not seven_zip:
        pytest.skip("7-Zip is required to create the ZIP package sample")

    archive = tmp_path / f"extension-check.{suffix}"
    preserved_name = "README.md"
    if suffix == "apk":
        apk_sample = Path(os.environ.get(
            "PEARE_ANDROID_APK_SAMPLE",
            Path.home() / "Documents" / "inerente a" / "Peare" /
            "validation-artifacts" / "android-apk" /
            "org.fdroid.fdroid_1023052.apk"))
        if not apk_sample.is_file():
            pytest.skip(f"Missing real Android APK sample: {apk_sample}")
        shutil.copyfile(apk_sample, archive)
        preserved_name = "AndroidManifest.xml"
    else:
        created = subprocess.run(
            [seven_zip, "a", "-tzip", "-mx=0", str(archive), "README.md"],
            cwd=ROOT, capture_output=True, text=True, timeout=60)
        assert created.returncode == 0, created.stdout + created.stderr
    with zipfile.ZipFile(archive) as original:
        original_names = set(original.namelist())
        preserved_payload = original.read(preserved_name)

    source = ROOT / "THIRDPARTY.md"
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"peare-added.txt"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to(
            writer, str(archive).encode(), None, None, 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), b"CUD-CREATED.TXT") == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(ROOT / "AGENTS.md").encode(), b"CUD-CREATED.TXT") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"CUD-CREATED.TXT", b"CUD-FINAL.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"peare-added.txt", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as updated:
        assert updated.testzip() is None
        assert updated.read(preserved_name) == preserved_payload
        assert updated.read("CUD-FINAL.TXT") == (ROOT / "AGENTS.md").read_bytes()
        assert set(updated.namelist()) == (original_names - {"peare-added.txt"}) | {
            "CUD-FINAL.TXT"}
    validation = subprocess.run(
        [seven_zip, "t", str(archive)], capture_output=True, text=True,
        timeout=60)
    assert validation.returncode == 0, validation.stdout + validation.stderr
    opened = peare_lib.open_file(str(archive))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert rows[preserved_name] == hashlib.sha256(preserved_payload).hexdigest()
        assert rows["CUD-FINAL.TXT"] == hashlib.sha256(
            (ROOT / "AGENTS.md").read_bytes()).hexdigest()
        assert "peare-added.txt" not in rows
    finally:
        opened.close()

def _create_windows_zip_package_sample(seven_zip, tmp_path, suffix):
    stage = tmp_path / f"{suffix}-package-source"
    stage.mkdir()
    shutil.copyfile(ROOT / "README.md", stage / "payload.txt")
    members = {"payload.txt": stage / "payload.txt"}
    if suffix in ("appx", "msix"):
        (stage / "AppxManifest.xml").write_text(
            '<Package><Identity Name="Peare.Test" Publisher="CN=Peare" '
            'Version="1.0.0.0" ProcessorArchitecture="x64" />'
            '<Properties><DisplayName>Peare Test</DisplayName></Properties>'
            '</Package>', encoding="utf-8")
        (stage / "[Content_Types].xml").write_text(
            '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
            '<Default Extension="txt" ContentType="text/plain"/></Types>',
            encoding="utf-8")
        members.update({
            "AppxManifest.xml": stage / "AppxManifest.xml",
            "[Content_Types].xml": stage / "[Content_Types].xml",
        })
    elif suffix == "appxbundle":
        metadata = stage / "AppxMetadata"
        metadata.mkdir()
        (metadata / "AppxBundleManifest.xml").write_text(
            '<Bundle><Identity Name="Peare.Test.Bundle" Version="1.0.0.0" '
            'Publisher="CN=Peare"/><Packages><Package Type="application" '
            'Version="1.0.0.0" Architecture="x64" FileName="Peare.appx"/>'
            '</Packages></Bundle>', encoding="utf-8")
        members["AppxMetadata/AppxBundleManifest.xml"] = \
            metadata / "AppxBundleManifest.xml"
        shutil.copyfile(ROOT / "README.md", stage / "Peare.appx")
        members["Peare.appx"] = stage / "Peare.appx"
    elif suffix == "xap":
        (stage / "AppManifest.xaml").write_text(
            '<Deployment RuntimeVersion="4.0.50826.0" '
            'EntryPointAssembly="Peare.Test" EntryPointType="Peare.Test.App"/>',
            encoding="utf-8")
        members["AppManifest.xaml"] = stage / "AppManifest.xaml"
    members["payload.txt"] = stage / "payload.txt"

    archive = tmp_path / f"external-package.{suffix}"
    names = list(members)
    created = subprocess.run(
        [seven_zip, "a", "-tzip", "-mx=0", str(archive), *names],
        cwd=stage, capture_output=True, text=True, timeout=60)
    assert created.returncode == 0, created.stdout + created.stderr
    return archive

@pytest.mark.parametrize("suffix", ("appx", "msix", "appxbundle", "xap"))
def test_windows_zip_package_alias_create_and_full_abi_cud(
        writer_lib, peare_lib, tmp_path, suffix):
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required to create the package sample")
    sample = _create_windows_zip_package_sample(seven_zip, tmp_path, suffix)
    with zipfile.ZipFile(sample) as source_archive:
        source_items = [(name, source_archive.read(name))
                        for name in source_archive.namelist()]
    payload_root = tmp_path / f"{suffix}-create-inputs"
    payload_root.mkdir()
    inputs = []
    for name, payload in source_items:
        target = payload_root.joinpath(*PurePosixPath(name).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        inputs.append((str(target).encode(), name.encode()))
    created_archive = tmp_path / f"peare-created.{suffix}"
    input_array = (_WriterInputFile * len(inputs))(*inputs)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_to(
            writer, str(created_archive).encode(), b"zip", b"deflate", 0,
            input_array, len(inputs)) == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    validation = subprocess.run(
        [seven_zip, "t", str(created_archive)], capture_output=True,
        text=True, timeout=60)
    assert validation.returncode == 0, validation.stdout + validation.stderr

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(created_archive).encode()) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), b"CUD-ADD.TXT") == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(ROOT / "AGENTS.md").encode(), b"payload.txt") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"CUD-ADD.TXT", b"CUD-RENAMED.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"payload.txt", 0) == 0
        finish_status = writer_lib.peare_writer_update_finish(writer)
        assert finish_status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(created_archive) as updated:
        assert updated.testzip() is None
        assert updated.read("CUD-RENAMED.TXT") == (ROOT / "README.md").read_bytes()
        assert "payload.txt" not in updated.namelist()
        if suffix in ("appx", "msix"):
            assert updated.read("AppxManifest.xml")
        if suffix == "appxbundle":
            assert "AppxMetadata/AppxBundleManifest.xml" in updated.namelist()
        if suffix == "xap":
            assert "AppManifest.xaml" in updated.namelist()
    opened = peare_lib.open_file(str(created_archive))
    assert opened is not None
    try:
        expected_format = {
            "appx": "APPX", "msix": "MSIX",
            "appxbundle": "APPXBUNDLE", "xap": "XAP",
        }[suffix]
        assert opened.container_format() == expected_format
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert rows["CUD-RENAMED.TXT"] == hashlib.sha256(
            (ROOT / "README.md").read_bytes()).hexdigest()
    finally:
        opened.close()

@pytest.mark.parametrize("view", ("tree", "details"))
@pytest.mark.parametrize("suffix", ("appx", "msix", "appxbundle", "xap"))
def test_windows_zip_package_alias_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, suffix, view):
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required to create the package sample")
    sample = _create_windows_zip_package_sample(seven_zip, tmp_path, suffix)
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(sample), view, gui_root="")

def test_android_apk_create_from_real_sample_contents(
        writer_lib, peare_lib, tmp_path):
    apk_sample = Path(os.environ.get(
        "PEARE_ANDROID_APK_SAMPLE",
        Path.home() / "Documents" / "inerente a" / "Peare" /
        "validation-artifacts" / "android-apk" /
        "org.fdroid.fdroid_1023052.apk"))
    if not apk_sample.is_file():
        pytest.skip(f"Missing real Android APK sample: {apk_sample}")
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required to validate the created APK container")

    payload_root = tmp_path / "apk-payload"
    payload_root.mkdir()
    inputs = []
    with zipfile.ZipFile(apk_sample) as source:
        for member in source.infolist():
            if member.is_dir():
                continue
            name = PurePosixPath(member.filename)
            assert not name.is_absolute() and all(
                part not in ("", ".", "..") for part in name.parts)
            upper_name = member.filename.upper()
            if upper_name.startswith("META-INF/") and (
                    upper_name.endswith((".SF", ".RSA", ".DSA", ".EC")) or
                    upper_name == "META-INF/MANIFEST.MF"):
                continue
            target = payload_root.joinpath(*name.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.read(member))
            inputs.append((str(target).encode(), member.filename.encode()))
    assert any(name == b"AndroidManifest.xml" for _, name in inputs)

    archive = tmp_path / "created-from-real-apk-contents.apk"
    input_array = (_WriterInputFile * len(inputs))(*inputs)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_to(
            writer, str(archive).encode(), b"zip", b"deflate", 0,
            input_array, len(inputs)) == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), b"CUD-CREATED.TXT") == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(ROOT / "AGENTS.md").encode(), b"CUD-CREATED.TXT") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"CUD-CREATED.TXT", b"CUD-FINAL.TXT") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(ROOT / "README.md").encode(), b"DELETE-ME.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"DELETE-ME.TXT", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as created:
        assert created.testzip() is None
        assert "AndroidManifest.xml" in created.namelist()
        assert created.read("AndroidManifest.xml") == \
            (payload_root / "AndroidManifest.xml").read_bytes()
        assert created.read("CUD-FINAL.TXT") == (ROOT / "AGENTS.md").read_bytes()
        assert "DELETE-ME.TXT" not in created.namelist()
    checked = subprocess.run(
        [seven_zip, "t", str(archive)], capture_output=True, text=True, timeout=120)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    opened = peare_lib.open_file(str(archive))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        assert hashlib.sha256(
            (payload_root / "AndroidManifest.xml").read_bytes()).hexdigest() == \
            rows["AndroidManifest.xml"]
        assert rows["CUD-FINAL.TXT"] == hashlib.sha256(
            (ROOT / "AGENTS.md").read_bytes()).hexdigest()
        assert "DELETE-ME.TXT" not in rows
    finally:
        opened.close()

@pytest.mark.parametrize("compression", ["store", "deflate"])
def test_zip_directory_only_update_commits(
        writer_lib, peare_lib, tmp_path, compression):
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        seven_zip = str(candidate) if candidate.is_file() else None
    if not seven_zip:
        pytest.skip("7-Zip is required to create independent ZIP samples")

    archive = tmp_path / f"directory-only-{compression}.zip"
    level = "0" if compression == "store" else "6"
    created = subprocess.run(
        [seven_zip, "a", "-tzip", f"-mx={level}", str(archive),
         "README.md", "AGENTS.md"], cwd=ROOT, capture_output=True,
        text=True, timeout=60)
    assert created.returncode == 0, created.stdout + created.stderr
    source_hash = _sha256_file(archive)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0
        assert writer_lib.peare_writer_create_directory(
            writer, b"DIRECTORY-ONLY") == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(archive) as updated:
        assert updated.testzip() is None
        assert "DIRECTORY-ONLY/" in updated.namelist()
        assert updated.read("README.md") == (ROOT / "README.md").read_bytes()
        assert updated.read("AGENTS.md") == (ROOT / "AGENTS.md").read_bytes()
    opened = peare_lib.open_file(str(archive))
    assert opened is not None
    try:
        paths = {row["path"] for row in opened.normalized_tree()}
        assert "DIRECTORY-ONLY" in paths
        assert "README.md" in paths and "AGENTS.md" in paths
    finally:
        opened.close()
    assert _sha256_file(archive) != source_hash


@pytest.mark.parametrize("sample", _zip_shrink_samples_from_env(),
                         ids=lambda path: path.name if path else "samples-not-configured")
def test_zip_shrink_real_sample_cud(writer_lib, tmp_path, sample):
    if sample is None or not sample.is_file():
        pytest.skip("Set PEARE_ZIP_SHRINK_TEST_IMAGES to local Shrink ZIP samples")
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        if candidate.is_file():
            seven_zip = str(candidate)
    if not seven_zip:
        pytest.skip("7-Zip is required to verify the ZIP sample")

    source_hash = _sha256_file(sample)
    image = tmp_path / sample.name
    shutil.copy2(sample, image)
    with zipfile.ZipFile(image) as archive:
        shrink_entries = [info.filename for info in archive.infolist()
                          if info.compress_type == 1 and not info.is_dir()]
    assert shrink_entries
    _assert_archive_duplicate_create_is_atomic(
        writer_lib, image, shrink_entries[0], tmp_path)
    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1
    assert writer_lib.peare_writer_update_target_count(str(image).encode()) == 1

    add_source = ROOT / "THIRDPARTY.md"
    replace_source = ROOT / "README.md"
    added_name = b"PEARE-ADDED.TXT"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(image).encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(add_source).encode(), added_name) == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(replace_source).encode(), added_name) == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, added_name, b"PEARE-FINAL.TXT") == 0
        assert writer_lib.peare_writer_create_empty_file(
            writer, b"PEARE-EMPTY.TXT") == 0
        assert writer_lib.peare_writer_create_directory(
            writer, b"PEARE-REMOVE") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(add_source).encode(), b"PEARE-REMOVE/CHILD.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"PEARE-EMPTY.TXT", 0) == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"PEARE-REMOVE", 1) == 0
        if len(shrink_entries) > 1:
            assert writer_lib.peare_writer_delete_entry(
                writer, shrink_entries[1].encode(), 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(image) as updated:
        assert "PEARE-FINAL.TXT" in updated.namelist()
        assert shrink_entries[0] in updated.namelist()
        assert added_name.decode() not in updated.namelist()
        if len(shrink_entries) > 1:
            assert shrink_entries[1] not in updated.namelist()
        assert any(info.compress_type == 1 for info in updated.infolist())

    for member, expected in (("PEARE-FINAL.TXT", replace_source.read_bytes()),):
        extracted = subprocess.run(
            [seven_zip, "e", "-so", str(image), member],
            capture_output=True, timeout=120, check=False)
        assert extracted.returncode == 0, extracted.stderr.decode(errors="replace")
        assert hashlib.sha256(extracted.stdout).digest() == hashlib.sha256(expected).digest()
    validation = subprocess.run(
        [seven_zip, "t", str(image)], capture_output=True, text=True,
        timeout=120, check=False)
    assert validation.returncode == 0, validation.stdout + validation.stderr
    assert _sha256_file(sample) == source_hash

def test_zip_shrink_rename_latency(writer_lib, tmp_path):
    configured = _zip_shrink_samples_from_env()
    sample = next((path for path in configured
                   if path is not None and path.is_file()), None)
    if sample is None:
        pytest.skip("Set PEARE_ZIP_SHRINK_TEST_IMAGES to local Shrink ZIP samples")
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        if candidate.is_file():
            seven_zip = str(candidate)
    if not seven_zip:
        pytest.skip("7-Zip is required for independent Shrink ZIP validation")

    source_hash = _sha256_file(sample)
    with zipfile.ZipFile(sample) as archive:
        shrink_name = next(info.filename for info in archive.infolist()
                           if info.compress_type == 1 and not info.is_dir())
    original_payload = _zip_raw_member_payload(sample, shrink_name)
    image = tmp_path / sample.name
    shutil.copyfile(sample, image)
    measurements = []

    for run in range(3):
        target_name = f"PEARE{run}.SHR"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_begin(
                writer, str(image).encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_rename_entry(
                writer, shrink_name.encode(), target_name.encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, rename_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, rename_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)
        with zipfile.ZipFile(image) as updated:
            assert updated.getinfo(target_name).compress_type == 1
        assert _zip_raw_member_payload(image, target_name) == original_payload
        checked = subprocess.run(
            [seven_zip, "t", str(image)], capture_output=True, text=True,
            timeout=60)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        shrink_name = target_name

    print("ZIP Shrink rename seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 2.0
    assert _sha256_file(sample) == source_hash


def test_zip_shrink_large_add_latency_and_7zip_readback(writer_lib, tmp_path):
    configured = _zip_shrink_samples_from_env()
    sample = next((path for path in configured
                   if path is not None and path.is_file()), None)
    if sample is None:
        pytest.skip("Set PEARE_ZIP_SHRINK_TEST_IMAGES to local Shrink ZIP samples")
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        if candidate.is_file():
            seven_zip = str(candidate)
    if not seven_zip:
        pytest.skip("7-Zip is required for independent Shrink ZIP validation")

    source_hash = _sha256_file(sample)
    with zipfile.ZipFile(sample) as original:
        shrink_name = next(info.filename for info in original.infolist()
                           if info.compress_type == 1 and not info.is_dir())
    shrink_payload = _zip_raw_member_payload(sample, shrink_name)
    payload = tmp_path / "zip-shrink-large-add-payload.bin"
    payload.write_bytes(os.urandom(16 * 1024 * 1024))
    payload_hash = hashlib.sha256(payload.read_bytes()).hexdigest()
    measurements = []

    for run in range(3):
        image = tmp_path / f"zip-shrink-large-add-{run}.zip"
        shutil.copyfile(sample, image)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_files_begin(
                writer, str(image).encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started

            started = time.perf_counter()
            status = writer_lib.peare_writer_add_file(
                writer, str(payload).encode(), b"LARGE-PAYLOAD.BIN")
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add_seconds = time.perf_counter() - started

            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            measurements.append((begin_seconds, add_seconds, finish_seconds))
        finally:
            writer_lib.peare_writer_destroy(writer)

        with zipfile.ZipFile(image) as updated:
            assert updated.getinfo(shrink_name).compress_type == 1
            assert updated.read("LARGE-PAYLOAD.BIN") == payload.read_bytes()
        assert _zip_raw_member_payload(image, shrink_name) == shrink_payload
        checked = subprocess.run(
            [seven_zip, "t", str(image)], capture_output=True, text=True,
            timeout=120)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        assert hashlib.sha256(payload.read_bytes()).hexdigest() == payload_hash

    print("ZIP Shrink 16 MiB add seconds:", measurements)
    assert max(row[1] for row in measurements) < 5.0
    assert max(row[2] for row in measurements) < 10.0
    assert _sha256_file(sample) == source_hash


def test_zip64_external_tool_sample_cud(writer_lib, tmp_path):
    sample = _zip64_sample_from_env()
    if sample is None or not sample.is_file():
        pytest.skip("Set PEARE_ZIP64_TEST_IMAGE to a local ZIP64 archive")
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        if candidate.is_file():
            seven_zip = str(candidate)
    if not seven_zip:
        pytest.skip("7-Zip is required to verify the ZIP64 sample")

    source_hash = _sha256_file(sample)
    image = tmp_path / sample.name
    shutil.copy2(sample, image)
    data = image.read_bytes()
    assert struct.pack("<I", 0x06064B50) in data[-128:]
    assert struct.pack("<I", 0x07064B50) in data[-128:]
    with zipfile.ZipFile(image) as original:
        assert len(original.infolist()) == 65536
        assert original.testzip() is None
        original_names = set(original.namelist())
    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1
    assert writer_lib.peare_writer_update_target_count(str(image).encode()) == 1

    added_source = ROOT / "THIRDPARTY.md"
    replacement_source = ROOT / "README.md"
    added_name = b"PEARE-ZIP64-ADDED.TXT"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(image).encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(added_source).encode(), added_name) == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement_source).encode(), b"FILE00001.TXT") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, added_name, b"PEARE-ZIP64-FINAL.TXT") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"FILE00002.TXT", 0) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    with zipfile.ZipFile(image) as updated:
        assert updated.testzip() is None
        names = set(updated.namelist())
        assert len(names) == 65536
        assert b"PEARE-ZIP64-FINAL.TXT".decode() in names
        assert added_name.decode() not in names
        assert "FILE00002.TXT" not in names
        assert names - {"PEARE-ZIP64-FINAL.TXT"} >= \
            original_names - {"FILE00001.TXT", "FILE00002.TXT"}
        assert updated.read("FILE00001.TXT") == replacement_source.read_bytes()
        assert updated.read("PEARE-ZIP64-FINAL.TXT") == added_source.read_bytes()
    validation = subprocess.run(
        [seven_zip, "t", str(image)], capture_output=True, text=True,
        timeout=180, check=False)
    assert validation.returncode == 0, validation.stdout + validation.stderr
    assert _sha256_file(sample) == source_hash


@pytest.mark.parametrize("sample", _zip64_large_samples_from_env(),
                         ids=lambda path: path.name if path else "no-large-samples")
def test_large_local_zip64_samples_are_discovered_read_only(writer_lib, sample):
    if sample is None or not sample.is_file():
        pytest.skip("Set PEARE_ZIP64_LARGE_TEST_IMAGES to local ZIP64 samples")
    before = sample.stat()
    assert writer_lib.peare_writer_can_add_files(str(sample).encode()) == 1
    assert writer_lib.peare_writer_update_target_count(str(sample).encode()) == 1
    after = sample.stat()
    assert (after.st_size, after.st_mtime_ns) == (before.st_size, before.st_mtime_ns)

_HANDLE = C.c_void_p

def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.digest()

def _disk_image_members(image):
    members = [image]
    if image.suffix.casefold() != ".vmdk":
        return members
    descriptor = image.read_bytes()
    if b"# Disk DescriptorFile" not in descriptor[:1024]:
        return members
    for line in descriptor.decode("latin1").splitlines():
        match = re.match(
            r'^(?:RW|RDONLY|NOACCESS)\s+\d+\s+\S+\s+"([^"]+)"',
            line.strip())
        if match:
            members.append(image.parent / match.group(1))
    return members

def _copy_file_sha256(source_path, destination_path):
    digest = hashlib.sha256()
    with open(source_path, "rb") as source, open(destination_path, "wb") as destination:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            destination.write(chunk)
            digest.update(chunk)
    shutil.copystat(source_path, destination_path)
    return digest.digest()

def _fsverity_metadata(payload, block_size):
    hashes = [
        hashlib.sha256(
            payload[offset:offset + block_size].ljust(block_size, b"\0")
        ).digest()
        for offset in range(0, len(payload), block_size)
    ]
    levels = []
    while len(hashes) > 1:
        level = bytearray()
        parents = []
        fanout = block_size // 32
        for first in range(0, len(hashes), fanout):
            block = bytearray(block_size)
            group = hashes[first:first + fanout]
            for index, digest in enumerate(group):
                block[index * 32:(index + 1) * 32] = digest
            level.extend(block)
            parents.append(hashlib.sha256(block).digest())
        levels.append(bytes(level))
        hashes = parents

    descriptor = bytearray(256)
    descriptor[0:4] = bytes((1, 1, block_size.bit_length() - 1, 0))
    struct.pack_into("<Q", descriptor, 8, len(payload))
    if payload:
        descriptor[16:48] = hashes[0]
    return b"".join(reversed(levels)), bytes(descriptor)

def _ext4_block_map(debugfs, image, path, logical_block, env):
    result = subprocess.run(
        [debugfs, "-R", f"bmap {path} {logical_block}", str(image)],
        capture_output=True, text=True, timeout=60, env=env)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"(?m)^(\d+)\s*$", result.stdout + result.stderr)
    assert match, result.stdout + result.stderr
    return int(match.group(1))

def _ext4_read_logical(debugfs, image, path, offset, length, block_size, env):
    output = bytearray()
    with Path(image).open("rb") as source:
        while length:
            logical_block, block_offset = divmod(offset, block_size)
            count = min(length, block_size - block_offset)
            physical_block = _ext4_block_map(
                debugfs, image, path, logical_block, env)
            assert physical_block != 0
            source.seek(physical_block * block_size + block_offset)
            chunk = source.read(count)
            assert len(chunk) == count
            output.extend(chunk)
            offset += count
            length -= count
    return bytes(output)

def _sha256_file_range(path, offset, length):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        source.seek(offset)
        remaining = length
        while remaining:
            chunk = source.read(min(1024 * 1024, remaining))
            assert chunk
            digest.update(chunk)
            remaining -= len(chunk)
    return digest.digest()

def _opener_root_snapshot(peare_lib, image):
    opener = peare_lib.open_file(str(image))
    assert opener is not None
    snapshot = {}
    api = peare_lib._lib
    try:
        payloads_hashed = 0
        for folder in range(min(opener.folder_count(), 8)):
            count = min(opener.resource_count(folder), 128)
            for index in range(count):
                resource = _HANDLE()
                if api.peare_opener_open_resource_at(
                        opener._h, folder, index, C.byref(resource)) != 0:
                    continue
                try:
                    context = po._Context()
                    if api.peare_resource_get_context(resource, C.byref(context)) != 0:
                        continue
                    name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                    size = int(context.data_size)
                    is_container = bool(context.is_container)
                    api.peare_resource_context_free(C.byref(context))
                    digest = ""
                    if not is_container and size <= 1024 * 1024 and payloads_hashed < 16:
                        blob = po._Blob()
                        if api.peare_resource_get_payload(resource, C.byref(blob)) == 0:
                            data = blob.to_bytes()
                            digest = hashlib.sha256(data).hexdigest()
                            payloads_hashed += 1
                            api.peare_blob_free(C.byref(blob))
                    snapshot[f"{folder}/{name}"] = (size, is_container, digest)
                finally:
                    api.peare_resource_destroy(resource)
        return snapshot
    finally:
        opener.close()

class _WriterFormat(C.Structure):
    _fields_ = [("id", C.c_char_p), ("display_name", C.c_char_p),
                ("extension", C.c_char_p)]

class _WriterVariant(C.Structure):
    _fields_ = [("id", C.c_char_p), ("display_name", C.c_char_p)]

class _WriterInputFile(C.Structure):
    _fields_ = [("source_path_utf8", C.c_char_p), ("entry_name_utf8", C.c_char_p)]

class _WriterCreateOptions(C.Structure):
    _fields_ = [("format_id_utf8", C.c_char_p),
                ("variant_id_utf8", C.c_char_p),
                ("filesystem_id_utf8", C.c_char_p)]

def _writer_path():
    override = os.environ.get("PEARE_WRITER_LIB")
    if override and Path(override).is_file():
        return Path(override)
    names = ("PeareWriter.dll", "libPeareWriter.so", "libPeareWriter.dylib")
    for build in ("build-windows/Release", "build/Release", "build"):
        for name in names:
            candidate = ROOT / build / name
            if candidate.is_file():
                return candidate
    return None

@pytest.fixture(scope="session")
def writer_lib():
    path = _writer_path()
    if path is None:
        pytest.skip("PeareWriter library not found; build it or set PEARE_WRITER_LIB")
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(path.resolve().parent))
    lib = C.CDLL(str(path))
    lib.peare_writer_create.argtypes = [C.POINTER(_HANDLE)]
    lib.peare_writer_create.restype = C.c_int
    lib.peare_writer_destroy.argtypes = [_HANDLE]
    lib.peare_writer_create_format_count.restype = C.c_size_t
    lib.peare_writer_get_create_format.argtypes = [C.c_size_t, C.POINTER(_WriterFormat)]
    lib.peare_writer_get_create_format.restype = C.c_int
    lib.peare_writer_create_variant_count.argtypes = [C.c_char_p]
    lib.peare_writer_create_variant_count.restype = C.c_size_t
    lib.peare_writer_get_create_variant.argtypes = [
        C.c_char_p, C.c_size_t, C.POINTER(_WriterVariant)]
    lib.peare_writer_get_create_variant.restype = C.c_int
    lib.peare_writer_add_files_to.argtypes = [
        _HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.c_size_t,
        C.POINTER(_WriterInputFile), C.c_size_t]
    lib.peare_writer_add_files_to.restype = C.c_int
    lib.peare_writer_add_files_to_ex.argtypes = [
        _HANDLE, C.c_char_p, C.POINTER(_WriterCreateOptions), C.c_size_t,
        C.POINTER(_WriterInputFile), C.c_size_t]
    lib.peare_writer_add_files_to_ex.restype = C.c_int
    lib.peare_writer_create_filesystem_count.argtypes = [C.c_char_p]
    lib.peare_writer_create_filesystem_count.restype = C.c_size_t
    lib.peare_writer_get_create_filesystem.argtypes = [
        C.c_char_p, C.c_size_t, C.POINTER(_WriterVariant)]
    lib.peare_writer_get_create_filesystem.restype = C.c_int
    lib.peare_writer_create_begin.argtypes = [_HANDLE, C.c_char_p, C.c_char_p]
    lib.peare_writer_create_begin.restype = C.c_int
    lib.peare_writer_create_begin_variant.argtypes = [
        _HANDLE, C.c_char_p, C.c_char_p, C.c_char_p]
    lib.peare_writer_create_begin_variant.restype = C.c_int
    lib.peare_writer_finish.argtypes = [_HANDLE]
    lib.peare_writer_finish.restype = C.c_int
    lib.peare_writer_can_add_files.argtypes = [C.c_char_p]
    lib.peare_writer_can_add_files.restype = C.c_int
    lib.peare_writer_get_update_capabilities.argtypes = [
        C.c_char_p, C.c_size_t, C.POINTER(C.c_uint32)]
    lib.peare_writer_get_update_capabilities.restype = C.c_int
    lib.peare_writer_update_target_count.argtypes = [C.c_char_p]
    lib.peare_writer_update_target_count.restype = C.c_size_t
    lib.peare_writer_get_update_target_name.argtypes = [
        C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t]
    lib.peare_writer_get_update_target_name.restype = C.c_int
    lib.peare_writer_update_begin.argtypes = [_HANDLE, C.c_char_p]
    lib.peare_writer_update_begin.restype = C.c_int
    lib.peare_writer_update_begin_target.argtypes = [_HANDLE, C.c_char_p, C.c_size_t]
    lib.peare_writer_update_begin_target.restype = C.c_int
    lib.peare_writer_create_empty_file.argtypes = [_HANDLE, C.c_char_p]
    lib.peare_writer_create_empty_file.restype = C.c_int
    lib.peare_writer_create_file.argtypes = [_HANDLE, C.c_char_p, C.c_char_p]
    lib.peare_writer_create_file.restype = C.c_int
    lib.peare_writer_create_directory.argtypes = [_HANDLE, C.c_char_p]
    lib.peare_writer_create_directory.restype = C.c_int
    lib.peare_writer_replace_file.argtypes = [_HANDLE, C.c_char_p, C.c_char_p]
    lib.peare_writer_replace_file.restype = C.c_int
    lib.peare_writer_replace_entry_by_id.argtypes = [
        _HANDLE, C.c_char_p, C.c_char_p, C.c_int64]
    lib.peare_writer_replace_entry_by_id.restype = C.c_int
    lib.peare_writer_enable_fsverity.argtypes = [_HANDLE, C.c_char_p]
    lib.peare_writer_enable_fsverity.restype = C.c_int
    lib.peare_writer_rename_entry.argtypes = [_HANDLE, C.c_char_p, C.c_char_p]
    lib.peare_writer_rename_entry.restype = C.c_int
    lib.peare_writer_rename_entry_by_id.argtypes = [
        _HANDLE, C.c_char_p, C.c_char_p, C.c_int64]
    lib.peare_writer_rename_entry_by_id.restype = C.c_int
    lib.peare_writer_delete_entry.argtypes = [_HANDLE, C.c_char_p, C.c_int]
    lib.peare_writer_delete_entry.restype = C.c_int
    lib.peare_writer_delete_entry_by_id.argtypes = [
        _HANDLE, C.c_char_p, C.c_int, C.c_int64]
    lib.peare_writer_delete_entry_by_id.restype = C.c_int
    lib.peare_writer_add_target_count.argtypes = [C.c_char_p]
    lib.peare_writer_add_target_count.restype = C.c_size_t
    lib.peare_writer_get_add_target_name.argtypes = [C.c_char_p, C.c_size_t,
                                                      C.c_char_p, C.c_size_t]
    lib.peare_writer_get_add_target_name.restype = C.c_int
    lib.peare_writer_add_files_begin.argtypes = [_HANDLE, C.c_char_p]
    lib.peare_writer_add_files_begin.restype = C.c_int
    lib.peare_writer_add_files_begin_target.argtypes = [_HANDLE, C.c_char_p, C.c_size_t]
    lib.peare_writer_add_files_begin_target.restype = C.c_int
    lib.peare_writer_add_file.argtypes = [_HANDLE, C.c_char_p, C.c_char_p]
    lib.peare_writer_add_file.restype = C.c_int
    lib.peare_writer_update_finish.argtypes = [_HANDLE]
    lib.peare_writer_update_finish.restype = C.c_int
    lib.peare_writer_zip_begin.argtypes = [_HANDLE, C.c_char_p]
    lib.peare_writer_zip_begin.restype = C.c_int
    lib.peare_writer_zip_add_directory.argtypes = [_HANDLE, C.c_char_p]
    lib.peare_writer_zip_add_directory.restype = C.c_int
    lib.peare_writer_zip_add_buffer.argtypes = [_HANDLE, C.c_char_p, C.c_void_p, C.c_size_t]
    lib.peare_writer_zip_add_buffer.restype = C.c_int
    lib.peare_writer_zip_add_file.argtypes = [_HANDLE, C.c_char_p, C.c_char_p]
    lib.peare_writer_zip_add_file.restype = C.c_int
    lib.peare_writer_zip_finish.argtypes = [_HANDLE]
    lib.peare_writer_zip_finish.restype = C.c_int
    lib.peare_writer_abort.argtypes = [_HANDLE]
    lib.peare_writer_last_error.argtypes = [_HANDLE]
    lib.peare_writer_last_error.restype = C.c_char_p
    return lib


def _cpio_tar_entries(tar, archive):
    result = subprocess.run([tar, "-tf", str(archive)], check=True, capture_output=True)
    return [line.decode("utf-8", "replace").rstrip("/").removeprefix("./")
            for line in result.stdout.splitlines() if line]


def _cpio_extract(tar, archive, entry):
    return subprocess.run([tar, "-xOf", str(archive), entry],
                          check=True, capture_output=True).stdout


def _assert_cpio_crc_checksums(archive):
    data = Path(archive).read_bytes()
    offset = 0
    while offset + 110 <= len(data):
        header = data[offset:offset + 110]
        assert header[:6] == b"070702"
        fields = [int(header[6 + index * 8:14 + index * 8], 16)
                  for index in range(13)]
        name_start = offset + 110
        name_end = name_start + fields[11]
        name = data[name_start:name_end]
        assert name.endswith(b"\0")
        payload_start = (name_end + 3) & ~3
        payload_end = payload_start + fields[6]
        assert payload_end <= len(data)
        assert sum(data[payload_start:payload_end]) & 0xFFFFFFFF == fields[12]
        offset = (payload_end + 3) & ~3
        if name[:-1] == b"TRAILER!!!":
            assert not any(data[offset:])
            return
    raise AssertionError("CPIO CRC trailer not found")


def test_cpio_create_variants_are_discoverable(writer_lib):
    expected = [
        (b"newc", b"SVR4 newc"),
        (b"crc", b"SVR4 CRC"),
        (b"odc", b"POSIX odc"),
        (b"afio-large", b"afio large ASCII"),
        (b"binary-le", b"Old binary little-endian"),
        (b"binary-be", b"Old binary big-endian"),
    ]
    assert writer_lib.peare_writer_create_variant_count(b"cpio") == len(expected)
    actual = (_WriterVariant * len(expected))()
    assert all(writer_lib.peare_writer_get_create_variant(
        b"cpio", index, C.byref(actual[index])) == 0
        for index in range(len(expected)))
    assert [(variant.id, variant.display_name) for variant in actual] == expected


@pytest.mark.parametrize(("variant", "magic"), [
    ("newc", b"070701"), ("crc", b"070702"),
    ("odc", b"070707"), ("afio-large", b"070727"),
    ("binary-le", b"\xc7\x71"), ("binary-be", b"\x71\xc7")])
def test_cpio_create_roundtrip(writer_lib, peare_lib, tmp_path, variant, magic):
    tar = _bsdtar_executable() or shutil.which("tar.exe") or shutil.which("tar")
    if not tar:
        pytest.skip("An independent bsdtar-compatible reader is required")
    archive = tmp_path / f"created-{variant}.cpio"
    payload = tmp_path / "payload.txt"
    payload.write_bytes(b"CPIO newc writer payload\n")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_create_begin_variant(
            writer, b"cpio", variant.encode(), str(archive).encode())
        assert status == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(writer, b"folder") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(payload).encode(), b"folder/payload.txt") == 0
        assert writer_lib.peare_writer_create_empty_file(writer, b"empty.bin") == 0
        status = writer_lib.peare_writer_finish(writer)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    assert archive.read_bytes()[:len(magic)] == magic
    assert set(_cpio_tar_entries(tar, archive)) == {
        "folder", "folder/payload.txt", "empty.bin"}
    assert _cpio_extract(tar, archive, "folder/payload.txt") == payload.read_bytes()
    assert _cpio_extract(tar, archive, "empty.bin") == b""
    with peare_lib.open_file(str(archive)) as opener:
        assert opener.container_format() == "CPIO"
        assert opener.folder_count() > 0
    if variant == "crc":
        _assert_cpio_crc_checksums(archive)

    batch_archive = tmp_path / f"batch-created-{variant}.cpio"
    options = _WriterCreateOptions(b"cpio", variant.encode(), None)
    input_file = _WriterInputFile(str(payload).encode(), b"batch/payload.txt")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(batch_archive).encode(), C.byref(options), 0,
            C.byref(input_file), 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _cpio_tar_entries(tar, batch_archive) == ["batch/payload.txt"]
    assert _cpio_extract(tar, batch_archive, "batch/payload.txt") == payload.read_bytes()
    if variant == "crc":
        _assert_cpio_crc_checksums(batch_archive)


@pytest.mark.parametrize(("sample_name", "replace_name", "magic"), [
    ("ascii-3.0-1.src.cpio", "ascii.spec", b"070701"),
    ("pcmcia", "etc/pcmcia/config", b"070702"),
    ("cinema.cpi", "Cinema.c", b"070707"),
    ("libarchive/test_read_format_cpio_afio.cpio", "file1", b"070707"),
    ("libarchive/test_read_format_cpio_bin_le.cpio", "file1111222233334444", b"\xc7\x71"),
    ("libarchive/test_read_format_cpio_bin_be.cpio", "file1111222233334444", b"\x71\xc7")])
def test_cpio_cud_on_external_sample(
        writer_lib, tmp_path, sample_name, replace_name, magic):
    root = Path(os.environ.get(
        "PEARE_CPIO_SAMPLE_ROOT",
        Path.home() / "Documents" / "inerente a" / "Peare" / "cpio-samples"))
    sample = root / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external CPIO sample: {sample}")
    tar = _bsdtar_executable() or shutil.which("tar.exe") or shutil.which("tar")
    if not tar:
        pytest.skip("An independent bsdtar-compatible reader is required")
    assert sample.read_bytes()[:len(magic)] == magic
    if sample_name.endswith("test_read_format_cpio_afio.cpio"):
        assert b"070727" in sample.read_bytes()
    original_hash = hashlib.sha256(sample.read_bytes()).digest()
    archive = tmp_path / sample.name
    shutil.copy2(sample, archive)
    assert hashlib.sha256(sample.read_bytes()).digest() == original_hash
    assert replace_name in _cpio_tar_entries(tar, archive)

    replacement = tmp_path / "replacement.txt"
    replacement.write_bytes(b"replacement content\n")
    added = tmp_path / "added.txt"
    added.write_bytes(b"added content\n")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_update_begin(writer, str(archive).encode())
        assert status == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), replace_name.encode()) == 0
        assert writer_lib.peare_writer_create_directory(writer, b"writer-check") == 0
        assert writer_lib.peare_writer_add_file(
            writer, str(added).encode(), b"writer-check/added.txt") == 0
        assert writer_lib.peare_writer_create_empty_file(
            writer, b"writer-check/delete.bin") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, replace_name.encode(), b"writer-check/renamed.spec") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, b"writer-check/delete.bin", 0) == 0
        status = writer_lib.peare_writer_update_finish(writer)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    entries = set(_cpio_tar_entries(tar, archive))
    assert "writer-check" in entries
    assert "writer-check/renamed.spec" in entries
    assert "writer-check/added.txt" in entries
    assert "writer-check/delete.bin" not in entries
    assert _cpio_extract(tar, archive, "writer-check/renamed.spec") == replacement.read_bytes()
    assert _cpio_extract(tar, archive, "writer-check/added.txt") == added.read_bytes()
    if magic == b"070702":
        _assert_cpio_crc_checksums(archive)
    assert hashlib.sha256(sample.read_bytes()).digest() == original_hash


@pytest.mark.parametrize("variant", ["gnu", "bsd"])
def test_ar_create_and_full_cud(writer_lib, peare_lib, tmp_path, variant):
    ar = shutil.which("ar.exe") or shutil.which("ar")
    if not ar:
        pytest.skip("GNU ar is required as an independent archive oracle")
    archive = tmp_path / f"created-{variant}.a"
    source = tmp_path / "payload.txt"
    source.write_bytes(b"original ar payload\n")
    long_source = tmp_path / "long-payload.txt"
    long_source.write_bytes(b"long-name payload\n")
    empty_name = b"empty.bin"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"ar", variant.encode(), str(archive).encode()) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), b"payload.txt") == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(long_source).encode(), b"member name longer than fifteen.txt") == 0
        assert writer_lib.peare_writer_create_empty_file(writer, empty_name) == 0
        status = writer_lib.peare_writer_finish(writer)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    names = subprocess.run([ar, "t", str(archive)], check=True, capture_output=True).stdout
    assert names.decode("utf-8").splitlines() == [
        "payload.txt", "member name longer than fifteen.txt", "empty.bin"]
    extracted = tmp_path / "ar-extracted"
    extracted.mkdir()
    for name in ("payload.txt", "member name longer than fifteen.txt", "empty.bin"):
        subprocess.run([ar, "x", str(archive), name], cwd=extracted,
                       check=True, capture_output=True)
    assert (extracted / "payload.txt").read_bytes() == source.read_bytes()
    assert (extracted / "member name longer than fifteen.txt").read_bytes() == long_source.read_bytes()
    assert (extracted / "empty.bin").read_bytes() == b""
    with peare_lib.open_file(str(archive)) as opener:
        assert opener.container_format() == "AR"
        assert {row["path"] for row in opener.normalized_tree()} == {
            "payload.txt", "member name longer than fifteen.txt", "empty.bin"}

    replacement = tmp_path / "replacement.txt"
    replacement.write_bytes(b"updated ar payload\n")
    added = tmp_path / "added.txt"
    added.write_bytes(b"added ar payload\n")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, str(archive).encode()) == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), b"payload.txt") == 0
        assert writer_lib.peare_writer_add_file(
            writer, str(added).encode(), b"added.txt") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, b"member name longer than fifteen.txt", b"renamed.txt") == 0
        assert writer_lib.peare_writer_delete_entry(writer, empty_name, 0) == 0
        status = writer_lib.peare_writer_update_finish(writer)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    names = subprocess.run([ar, "t", str(archive)], check=True, capture_output=True).stdout
    assert names.decode("utf-8").splitlines() == ["payload.txt", "renamed.txt", "added.txt"]
    updated = tmp_path / "ar-updated"
    updated.mkdir()
    for name in ("payload.txt", "renamed.txt", "added.txt"):
        subprocess.run([ar, "x", str(archive), name], cwd=updated,
                       check=True, capture_output=True)
    assert (updated / "payload.txt").read_bytes() == replacement.read_bytes()
    assert (updated / "renamed.txt").read_bytes() == long_source.read_bytes()
    assert (updated / "added.txt").read_bytes() == added.read_bytes()
    assert writer_lib.peare_writer_can_add_files(str(archive).encode()) == 1


def test_ar_cud_preserves_members_from_real_gnu_archive(writer_lib, tmp_path):
    ar = shutil.which("ar.exe") or shutil.which("ar")
    sample = Path(
        Path.home() / "Documents" / "inerente a" / "Peare" / "Research tools"
        r"\lz4-1.10.0\dll\liblz4.dll.a")
    if not ar or not sample.is_file():
        pytest.skip("GNU ar and the independent local liblz4 archive are required")
    original_hash = _sha256_file(sample)
    archive = tmp_path / sample.name
    shutil.copy2(sample, archive)
    original_names = subprocess.run([ar, "t", str(sample)], check=True,
                                    capture_output=True).stdout.decode().splitlines()
    assert len(original_names) >= 3
    replacement = tmp_path / "replacement.o"
    replacement.write_bytes(b"replacement object payload\n")
    added = tmp_path / "added.o"
    added.write_bytes(b"added object payload\n")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, str(archive).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), original_names[0].encode()) == 0
        assert writer_lib.peare_writer_add_file(
            writer, str(added).encode(), b"added.o") == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, original_names[1].encode(), b"renamed.o") == 0
        assert writer_lib.peare_writer_delete_entry(
            writer, original_names[2].encode(), 0) == 0
        status = writer_lib.peare_writer_update_finish(writer)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    expected = list(original_names)
    expected[1] = "renamed.o"
    del expected[2]
    expected.append("added.o")
    actual = subprocess.run([ar, "t", str(archive)], check=True,
                            capture_output=True).stdout.decode().splitlines()
    assert actual == expected
    extracted = tmp_path / "real-ar-extract"
    extracted.mkdir()
    for name in (original_names[0], "renamed.o", "added.o"):
        subprocess.run([ar, "x", str(archive), name], cwd=extracted,
                       check=True, capture_output=True)
    assert (extracted / original_names[0]).read_bytes() == replacement.read_bytes()
    assert (extracted / "renamed.o").is_file()
    assert (extracted / "added.o").read_bytes() == added.read_bytes()
    assert _sha256_file(sample) == original_hash

def _fat_writer_add(writer_lib, image, source, entry_name):
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_begin(writer, str(image).encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_add_file(
            writer, str(source).encode(), entry_name.encode("utf-8")) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

def _fat_writer_add_target(writer_lib, image, source, entry_name, target_index):
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_begin_target(
            writer, str(image).encode(), target_index) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_add_file(
            writer, str(source).encode(), entry_name.encode("utf-8")) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

def _fat_path_matches(path, name):
    return path == name or path.endswith("/" + name)

def _fat_writer_cud(writer_lib, image, tmp_path, target_index=0,
                    base_directory="", source_file=None,
                    replacement_file=None):
    token = uuid.uuid4().hex[:6].upper()
    created_name = f"C{token}.BIN"
    final_name = f"R{token}.BIN"
    empty_name = f"E{token}.BIN"
    directory_name = f"D{token}"
    renamed_directory_name = f"Q{token}"
    child_name = f"{renamed_directory_name}/C{token}.BIN"
    prefix = f"{base_directory.rstrip('/')}/" if base_directory else ""
    created_path = prefix + created_name
    final_path = prefix + final_name
    empty_path = prefix + empty_name
    directory_path = prefix + directory_name
    renamed_directory_path = prefix + renamed_directory_name
    child_path = prefix + child_name
    source_payload = f"CUD create {token}".encode("ascii")
    replacement_payload = f"CUD replace {token}".encode("ascii")
    source = Path(source_file) if source_file else \
        tmp_path / f"cud-source-{token}.bin"
    replacement = Path(replacement_file) if replacement_file else \
        tmp_path / f"cud-replacement-{token}.bin"
    if source_file is None:
        source.write_bytes(source_payload)
    if replacement_file is None:
        replacement.write_bytes(replacement_payload)
    else:
        replacement_payload = replacement.read_bytes()

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), target_index) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), created_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), created_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, created_path.encode(), final_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_empty_file(
            writer, empty_path.encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, directory_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, directory_path.encode(), renamed_directory_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, final_path.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), child_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, empty_path.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, renamed_directory_path.encode(), 1) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(replacement).encode(), final_path.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    return {
        "created_name": created_path,
        "final_name": final_path,
        "empty_name": empty_path,
        "directory_name": directory_path,
        "renamed_directory_name": renamed_directory_path,
        "replacement_payload": replacement_payload,
    }

def _assert_vmdk_filesystem_oracles(
        image, cud, oracle_payload, filesystem_id, tmp_path,
        sector_size=512, use_qemu=True, initial_name=b"ORACLE.TXT"):
    qemu_img = os.environ.get("PEARE_QEMU_IMG") if use_qemu else None
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sleuthkit_bin:
        return
    fsstat = Path(sleuthkit_bin) / "fsstat.exe"
    recover_tool = Path(sleuthkit_bin) / "tsk_recover.exe"
    if not fsstat.is_file() or not recover_tool.is_file():
        return
    filesystem = filesystem_id.decode("ascii")
    tsk_filesystem = "ntfs" if filesystem == "ntfs" else "fat"
    image_type = "vmdk"
    oracle_image = image
    if qemu_img and Path(qemu_img).is_file():
        info = subprocess.run(
            [qemu_img, "info", "-f", "vmdk", "--output=json", str(image)],
            capture_output=True, text=True, timeout=60)
        assert info.returncode == 0, info.stdout + info.stderr
        assert json.loads(info.stdout)["format"] == "vmdk"
        oracle_image = tmp_path / f"{image.stem}-qemu-raw.img"
        conversion = subprocess.run(
            [qemu_img, "convert", "-f", "vmdk", "-O", "raw",
             str(image), str(oracle_image)], capture_output=True, text=True,
            timeout=180)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        image_type = "raw"
    check = subprocess.run(
        [str(fsstat), "-i", image_type, "-b", str(sector_size), "-f", tsk_filesystem,
         str(oracle_image)],
        capture_output=True, text=True, timeout=60)
    assert check.returncode == 0, check.stdout + check.stderr
    assert f"File System Type: {filesystem.upper()}" in check.stdout
    recovered = tmp_path / f"{image.stem}-tsk-recovered"
    recovered.mkdir()
    extraction = subprocess.run(
        [str(recover_tool), "-i", image_type, "-b", str(sector_size), "-f", tsk_filesystem,
         "-e", str(oracle_image), str(recovered)],
        capture_output=True, text=True, timeout=120)
    assert extraction.returncode == 0, extraction.stdout + extraction.stderr
    original = next(recovered.rglob(initial_name.decode("ascii")))
    final = next(recovered.rglob(Path(cud["final_name"]).name))
    assert _sha256_file(original) == hashlib.sha256(oracle_payload).digest()
    assert _sha256_file(final) == hashlib.sha256(
        cud["replacement_payload"]).digest()

def _assert_windows_vhdx_oracle(image, expected_sector_size=512):
    class _Guid(C.Structure):
        _fields_ = [("data1", C.c_uint32), ("data2", C.c_uint16),
                    ("data3", C.c_uint16), ("data4", C.c_ubyte * 8)]

    class _StorageType(C.Structure):
        _fields_ = [("device_id", C.c_uint32), ("vendor_id", _Guid)]

    class _OpenParameters(C.Structure):
        _fields_ = [("version", C.c_uint32), ("read_write_depth", C.c_uint32)]

    class _VirtualDiskSize(C.Structure):
        _fields_ = [("virtual_size", C.c_uint64), ("physical_size", C.c_uint64),
                    ("block_size", C.c_uint32), ("sector_size", C.c_uint32)]

    class _VirtualDiskInfo(C.Structure):
        _fields_ = [("version", C.c_uint32), ("size", _VirtualDiskSize)]

    if os.name != "nt":
        pytest.skip("The Windows virtual disk API is required")
    virtdisk = C.WinDLL("virtdisk.dll")
    kernel32 = C.WinDLL("kernel32.dll", use_last_error=True)
    storage_type = _StorageType(
        3,
        _Guid(0xEC984AEC, 0xA0F9, 0x47E9,
              (C.c_ubyte * 8)(0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B)))
    parameters = _OpenParameters(1, 0)
    handle = C.c_void_p()
    open_virtual_disk = virtdisk.OpenVirtualDisk
    open_virtual_disk.argtypes = [C.POINTER(_StorageType), C.c_wchar_p, C.c_uint32,
                                  C.c_uint32, C.POINTER(_OpenParameters),
                                  C.POINTER(C.c_void_p)]
    open_virtual_disk.restype = C.c_uint32
    status = open_virtual_disk(C.byref(storage_type), str(image), 0x00080000,
                               0, C.byref(parameters), C.byref(handle))
    assert status == 0, f"OpenVirtualDisk failed with Windows error {status}"
    try:
        info = _VirtualDiskInfo()
        info.version = 1
        info_size = C.c_uint32(C.sizeof(info))
        get_info = virtdisk.GetVirtualDiskInformation
        get_info.argtypes = [C.c_void_p, C.POINTER(C.c_uint32),
                             C.POINTER(_VirtualDiskInfo), C.POINTER(C.c_uint32)]
        get_info.restype = C.c_uint32
        status = get_info(handle, C.byref(info_size), C.byref(info), None)
        assert status == 0, f"GetVirtualDiskInformation failed with Windows error {status}"
        assert info.size.virtual_size > 0
        assert info.size.sector_size == expected_sector_size
    finally:
        kernel32.CloseHandle.argtypes = [C.c_void_p]
        kernel32.CloseHandle.restype = C.c_int
        assert kernel32.CloseHandle(handle)

def _assert_duplicate_create_is_atomic(
        writer_lib, image, image_members, tmp_path, target_index, entry_name):
    active_members = list(image_members)
    if image.suffix.casefold() == ".vmdk":
        descriptor = image.read_bytes().decode("latin1")
        extent_names = []
        for line in descriptor.splitlines():
            match = re.fullmatch(r'RW\s+\d+\s+(?:SPARSE|FLAT)\s+"([^"]+)"(?:\s+\d+)?',
                                 line.strip())
            if match:
                name = match.group(1)
                if Path(name).name == name and name not in (".", ".."):
                    extent_names.append(name)
        if extent_names:
            active_members = [image, *(image.parent / name for name in extent_names)]
    before = {path.name: _sha256_file(path) for path in active_members}
    prepared_before = {path.name for path in image.parent.iterdir()
                       if path.name.startswith(("peare-", ".peare-extent-"))}
    source = tmp_path / f"duplicate-rejection-{uuid.uuid4().hex}.bin"
    source.write_bytes(b"duplicate entry must not be published")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), target_index) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), entry_name.encode()) != 0
        assert writer_lib.peare_writer_update_finish(writer) != 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    after = {path.name: _sha256_file(path) for path in active_members}
    assert after == before
    prepared_after = {path.name for path in image.parent.iterdir()
                      if path.name.startswith(("peare-", ".peare-extent-"))}
    assert prepared_after <= prepared_before

def _assert_archive_duplicate_create_is_atomic(
        writer_lib, archive, entry_name, tmp_path):
    original_hash = _sha256_file(archive)
    source = tmp_path / f"archive-duplicate-{uuid.uuid4().hex}.bin"
    source.write_bytes(b"duplicate archive member must not be published")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(
            writer, str(archive).encode()) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), entry_name.encode("utf-8")) != 0
        writer_lib.peare_writer_update_finish(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(archive) == original_hash

def _fat_writer_targets(writer_lib, image):
    path = str(image).encode()
    targets = []
    for index in range(writer_lib.peare_writer_add_target_count(path)):
        name = C.create_string_buffer(256)
        assert writer_lib.peare_writer_get_add_target_name(
            path, index, name, len(name)) == 0
        targets.append(name.value.decode("utf-8"))
    return targets

def _fat_writer_target_index(writer_lib, image, marker):
    path = str(image).encode()
    for index in range(writer_lib.peare_writer_update_target_count(path)):
        name = C.create_string_buffer(256)
        assert writer_lib.peare_writer_get_add_target_name(
            path, index, name, len(name)) == 0
        if marker.encode() in name.value:
            return index
    raise AssertionError(_fat_writer_targets(writer_lib, image))

def _mbr_ext_partition(image, journaled):
    with open(image, "rb") as source:
        mbr = source.read(512)
        assert mbr[510:512] == b"\x55\xaa"
        matches = []
        for index in range(4):
            entry = mbr[446 + index * 16:462 + index * 16]
            start = int.from_bytes(entry[8:12], "little")
            sectors = int.from_bytes(entry[12:16], "little")
            if not start or not sectors:
                continue
            source.seek(start * 512 + 1024)
            superblock = source.read(1024)
            if len(superblock) != 1024:
                continue
            magic = int.from_bytes(superblock[56:58], "little")
            compat = int.from_bytes(superblock[92:96], "little")
            if magic == 0xEF53 and bool(compat & 0x4) == journaled:
                matches.append((start, sectors))
    assert len(matches) == 1, matches
    return matches[0]

def _tsk_recover_ext(sleuthkit_bin, image, partition_start, destination):
    destination.mkdir()
    command = [str(Path(sleuthkit_bin) / "tsk_recover.exe"), "-i", "raw",
               "-b", "512", "-o", str(partition_start), "-f", "ext", "-a",
               str(image), str(destination)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr
    return {
        path.relative_to(destination).as_posix(): _sha256_file(path)
        for path in destination.rglob("*") if path.is_file()
    }

def _tsk_inode_for_path(sleuthkit_bin, image, partition_start, path):
    result = subprocess.run(
        [str(Path(sleuthkit_bin) / "fls.exe"), "-i", "raw", "-b", "512",
         "-o", str(partition_start), "-f", "ext", "-r", "-p", str(image)],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    wanted = path.replace("\\", "/").lower()
    for line in result.stdout.splitlines():
        left, separator, name = line.partition(":")
        if separator and name.strip().replace("\\", "/").lower().endswith(wanted):
            match = re.search(r"\b(\d+)", left)
            if match:
                return int(match.group(1))
    raise AssertionError(f"TSK did not list {path}: {result.stdout}")

def _ext4_extent_tree_map(image, partition_start, inode_number):
    with Path(image).open("rb") as disk:
        partition_offset = partition_start * 512
        disk.seek(partition_offset + 1024)
        superblock = disk.read(1024)
        assert int.from_bytes(superblock[56:58], "little") == 0xEF53
        block_size = 1024 << int.from_bytes(superblock[24:28], "little")
        first_data_block = int.from_bytes(superblock[20:24], "little")
        inodes_per_group = int.from_bytes(superblock[40:44], "little")
        inode_size = int.from_bytes(superblock[88:90], "little")
        desc_size = int.from_bytes(superblock[254:256], "little") or 32
        features = int.from_bytes(superblock[96:100], "little")
        group = (inode_number - 1) // inodes_per_group
        index = (inode_number - 1) % inodes_per_group
        group_desc_offset = (partition_offset +
                             (first_data_block + 1) * block_size +
                             group * desc_size)
        disk.seek(group_desc_offset + 8)
        inode_table = int.from_bytes(disk.read(4), "little")
        if features & 0x80 and desc_size >= 64:
            disk.seek(group_desc_offset + 40)
            inode_table |= int.from_bytes(disk.read(4), "little") << 32
        disk.seek(partition_offset + inode_table * block_size + index * inode_size)
        inode = disk.read(inode_size)
    flags = int.from_bytes(inode[32:36], "little")
    assert flags & 0x80000, f"Inode {inode_number} does not use extents"
    root = inode[40:100]
    depth = int.from_bytes(root[6:8], "little")

    extents = []

    def collect_extents(node, node_depth):
        count = int.from_bytes(node[2:4], "little")
        if node_depth == 0:
            for entry_index in range(count):
                entry = node[12 + entry_index * 12:24 + entry_index * 12]
                logical = int.from_bytes(entry[0:4], "little")
                raw_length = int.from_bytes(entry[4:6], "little")
                physical = (int.from_bytes(entry[8:12], "little") |
                            (int.from_bytes(entry[6:8], "little") << 32))
                extents.append((logical, raw_length, physical))
            return
        for entry_index in range(count):
            entry = node[12 + entry_index * 12:24 + entry_index * 12]
            child_block = (int.from_bytes(entry[4:8], "little") |
                           (int.from_bytes(entry[8:10], "little") << 32))
            with Path(image).open("rb") as disk:
                disk.seek(partition_offset + child_block * block_size)
                child = disk.read(block_size)
            assert int.from_bytes(child[0:2], "little") == 0xF30A
            collect_extents(child, node_depth - 1)

    collect_extents(root, depth)
    return depth, extents

def _ext4_extent_tree_stats(image, partition_start, inode_number):
    depth, entries = _ext4_extent_tree_map(image, partition_start, inode_number)
    return depth, len(entries)

def _tsk_recover_fat(sleuthkit_bin, image, partition_start, destination):
    destination.mkdir()
    command = [str(Path(sleuthkit_bin) / "tsk_recover.exe"), "-i", "raw",
               "-b", "512", "-o", str(partition_start), "-f", "fat", "-a",
               str(image), str(destination)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr
    return {
        path.relative_to(destination).as_posix(): _sha256_file(path)
        for path in destination.rglob("*") if path.is_file()
    }

def test_chr_7207_fat32_cud_and_ext4_target(writer_lib, tmp_path):
    sample = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to the extracted CHR image")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")
    sample = Path(sample)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    partition_start = 2048
    ext4_partition_start = 69632
    ext4_partition_sectors = 188416
    mmls = subprocess.run(
        [str(Path(sleuthkit_bin) / "mmls.exe"), "-i", "raw", str(image)],
        capture_output=True, text=True, timeout=60)
    assert mmls.returncode == 0, mmls.stdout + mmls.stderr
    assert str(partition_start) in mmls.stdout
    assert str(ext4_partition_start) in mmls.stdout
    assert "Linux (0x83)" in mmls.stdout
    ext4_hash = _sha256_file_range(
        image, ext4_partition_start * 512, ext4_partition_sectors * 512)
    targets = _fat_writer_targets(writer_lib, image)
    assert len(targets) == 2 and "FAT" in targets[0] and "Ext4" in targets[1], targets
    before = _tsk_recover_fat(
        sleuthkit_bin, image, partition_start, tmp_path / "chr-7207-fat-before")
    assert before
    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, cud["final_name"])
    after = _tsk_recover_fat(
        sleuthkit_bin, image, partition_start, tmp_path / "chr-7207-fat-after")
    assert all(after.get(path) == digest for path, digest in before.items())
    final = next(path for path in after if _fat_path_matches(path, cud["final_name"]))
    assert after[final] == hashlib.sha256(cud["replacement_payload"]).digest()
    fatstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", str(partition_start), "-f", "fat", str(image)],
        capture_output=True, text=True, timeout=60)
    assert fatstat.returncode == 0, fatstat.stdout + fatstat.stderr
    assert "File System Type: FAT32" in fatstat.stdout
    extstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", str(ext4_partition_start), "-f", "ext", str(image)],
        capture_output=True, text=True, timeout=60)
    assert extstat.returncode == 0, extstat.stdout + extstat.stderr
    assert "File System Type: Ext4" in extstat.stdout
    assert len(_fat_writer_targets(writer_lib, image)) == 2
    assert _sha256_file_range(
        image, ext4_partition_start * 512, ext4_partition_sectors * 512) == ext4_hash
    assert _sha256_file(sample) == original_hash

def test_chr_7207_ext4_cud_on_disposable_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to the extracted CHR image")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")
    sample = Path(sample)
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    ext4_partition_start = 69632
    fat_region_hash = _sha256_file_range(image, 2048 * 512,
                                         (ext4_partition_start - 2048) * 512)
    before = _tsk_recover_ext(
        sleuthkit_bin, image, ext4_partition_start,
        tmp_path / "chr-7207-ext4-before")
    assert before
    target_index = _fat_writer_target_index(writer_lib, image, "Ext4")
    capabilities = C.c_uint32()
    assert writer_lib.peare_writer_get_update_capabilities(
        str(image).encode(), target_index, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f

    source = ROOT / "README.md"
    replacement = ROOT / "src/writer/formats/ExtVolumeUpdate.cpp"
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    token = uuid.uuid4().hex[:12].upper()
    directory = f"PEARE-{token}"
    removed_directory = f"PEARE-REMOVE-{token}"
    initial_name = f"{directory}/INITIAL.TXT"
    final_name = f"{directory}/FINAL.TXT"
    deleted_name = f"{directory}/DELETE.TXT"
    deleted_large_name = f"{directory}/DELETE-LARGE.TXT"

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), target_index) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, directory.encode()) == 0, writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), initial_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(replacement).encode(), initial_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_rename_entry(
            writer, initial_name.encode(), final_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_replace_file(
            writer, str(source).encode(), final_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(source).encode(), deleted_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, deleted_name.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_file(
            writer, str(replacement).encode(), deleted_large_name.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, deleted_large_name.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_create_directory(
            writer, removed_directory.encode()) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_delete_entry(
            writer, removed_directory.encode(), 0) == 0, \
            writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    rows = _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=2)
    by_path = {row["path"]: row for row in rows}
    final_path = next(path for path in by_path if path.endswith("/" + final_name))
    assert by_path[final_path]["sha256"] == source_hash
    assert not any(path.endswith("/" + name) for path in by_path
                   for name in (initial_name, deleted_name, deleted_large_name,
                                removed_directory))

    after = _tsk_recover_ext(
        sleuthkit_bin, image, ext4_partition_start,
        tmp_path / "chr-7207-ext4-after")
    assert all(after.get(path) == digest for path, digest in before.items())
    oracle_final = next(path for path in after if _fat_path_matches(path, final_name))
    assert after[oracle_final] == bytes.fromhex(source_hash)
    extstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", str(ext4_partition_start), "-f", "ext", str(image)],
        capture_output=True, text=True, timeout=60)
    assert extstat.returncode == 0, extstat.stdout + extstat.stderr
    assert "File System Type: Ext4" in extstat.stdout
    assert _sha256_file_range(image, 2048 * 512,
                              (ext4_partition_start - 2048) * 512) == fat_region_hash
    assert _sha256_file(sample) == original_hash


def test_chr_7207_ext4_large_file_add_vmdk_v2(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to the extracted CHR image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to the Sleuth Kit bin directory")

    sample = Path(sample_value)
    source_hash = _sha256_file(sample)
    partition_start = 69632
    before = _tsk_recover_ext(
        sleuthkit_bin, sample, partition_start, tmp_path / "ext4-large-before")
    image = tmp_path / "chr-7207-ext4-large.vmdk"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk",
         "-o", "subformat=monolithicSparse,zeroed_grain=on",
         str(sample), str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    with image.open("rb") as source:
        header = source.read(8)
    assert header[:4] == b"KDMV"
    assert int.from_bytes(header[4:8], "little") == 2

    payload = tmp_path / "ext4-large-payload.bin"
    payload_size = 16 * 1024 * 1024
    payload_pattern = b"Peare Ext4 large-file regression payload\n"
    payload.write_bytes(
        (payload_pattern * ((payload_size + len(payload_pattern) - 1) //
                            len(payload_pattern)))[:payload_size])
    entry_name = f"PEARE-LARGE-{uuid.uuid4().hex[:12].upper()}.BIN"
    target_index = _fat_writer_target_index(writer_lib, image, "Ext4")
    _fat_writer_add_target(writer_lib, image, payload, entry_name, target_index)

    roundtrip = tmp_path / "chr-7207-ext4-large-roundtrip.img"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "vmdk", "-O", "raw",
         str(image), str(roundtrip)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    after = _tsk_recover_ext(
        sleuthkit_bin, roundtrip, partition_start, tmp_path / "ext4-large-after")
    assert all(after.get(path) == digest for path, digest in before.items())
    recovered_path = next(path for path in after if _fat_path_matches(path, entry_name))
    assert after[recovered_path] == hashlib.sha256(payload.read_bytes()).digest()
    extstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", str(partition_start), "-f", "ext", str(roundtrip)],
        capture_output=True, text=True, timeout=60)
    assert extstat.returncode == 0, extstat.stdout + extstat.stderr
    assert "File System Type: Ext4" in extstat.stdout
    assert _sha256_file(sample) == source_hash

def test_chr_7207_ext4_splits_and_collapses_multilevel_extent_tree(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE")
    payload_value = os.environ.get("PEARE_EXT4_FRAGMENTED_PAYLOAD")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to the extracted CHR image")
    if not payload_value or not Path(payload_value).is_file():
        pytest.skip("Set PEARE_EXT4_FRAGMENTED_PAYLOAD to the local CHR ZIP sample")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "fls.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to the Sleuth Kit bin directory")
    e2fsprogs_bin = os.environ.get("PEARE_E2FSPROGS_BIN")
    if not e2fsprogs_bin or not (Path(e2fsprogs_bin) / "e2fsck.exe").is_file():
        pytest.skip("Set PEARE_E2FSPROGS_BIN to the e2fsprogs utility directory")

    sample = Path(sample_value)
    image = tmp_path / "chr-7207-ext4-multilevel.img"
    source_hash = _sha256_file(sample)
    assert _copy_file_sha256(sample, image) == source_hash
    partition_start = 69632
    before = _tsk_recover_ext(
        sleuthkit_bin, image, partition_start, tmp_path / "ext4-multilevel-before")
    one_block = ROOT / "docs" / "about" / "ROADMAP.md"
    assert one_block.stat().st_size <= 4096
    e2fsck = Path(e2fsprogs_bin) / "e2fsck.exe"
    tool_env = os.environ.copy()
    runtime_bin = Path(e2fsprogs_bin).parent.parent / "bin"
    tool_env["PATH"] = str(runtime_bin) + os.pathsep + tool_env.get("PATH", "")
    dd = runtime_bin / "dd.exe"
    assert dd.is_file(), f"Missing external partition-copy tool: {dd}"

    def assert_clean_filesystem():
        partition_image = tmp_path / "chr-7207-ext4-partition.raw"
        extract = subprocess.run(
            [str(dd), f"if={image}", f"of={partition_image}", "bs=512",
             "skip=69632", "count=188416"],
            capture_output=True, text=True, timeout=120, env=tool_env)
        assert extract.returncode == 0, extract.stdout + extract.stderr
        check = subprocess.run(
            [str(e2fsck), "-fn", str(partition_image)], capture_output=True,
            text=True, timeout=180, env=tool_env)
        assert check.returncode == 0, check.stdout + check.stderr

    def apply_batch(operations):
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        active = False
        try:
            target = _fat_writer_target_index(writer_lib, image, "Ext4")
            assert writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), target) == 0, \
                writer_lib.peare_writer_last_error(writer)
            active = True
            for operation in operations:
                if operation[0] == "create":
                    status = writer_lib.peare_writer_create_file(
                        writer, str(operation[1]).encode(), operation[2].encode())
                elif operation[0] == "replace":
                    status = writer_lib.peare_writer_replace_file(
                        writer, str(operation[1]).encode(), operation[2].encode())
                elif operation[0] == "delete":
                    status = writer_lib.peare_writer_delete_entry(
                        writer, operation[1].encode(), 0)
                elif operation[0] == "mkdir":
                    status = writer_lib.peare_writer_create_directory(
                        writer, operation[1].encode())
                else:
                    raise AssertionError(operation)
                assert status == 0, writer_lib.peare_writer_last_error(writer)
            assert writer_lib.peare_writer_update_finish(writer) == 0, \
                writer_lib.peare_writer_last_error(writer)
            active = False
        finally:
            if active:
                writer_lib.peare_writer_abort(writer)
            writer_lib.peare_writer_destroy(writer)

    directories = [f"EXT4FRAG/D{index:02d}" for index in range(32)]
    apply_batch([("mkdir", "EXT4FRAG")] +
                [("mkdir", directory) for directory in directories])

    fillers = [f"{directories[index // 100]}/F{index:04d}.BIN"
               for index in range(3200)]
    create_operations = [("create", one_block, name) for name in fillers]
    for start in range(0, len(create_operations), 64):
        apply_batch(create_operations[start:start + 64])
    delete_operations = [("delete", fillers[index])
                         for index in range(0, len(fillers), 2)]
    for start in range(0, len(delete_operations), 64):
        apply_batch(delete_operations[start:start + 64])

    medium_payload = ROOT / "src" / "decoder" / "resources" / "RT_BITMAP" / "RT_BITMAP.cpp"
    medium_path = "EXT4FRAG/MEDIUM.BIN"
    apply_batch([("create", medium_payload, medium_path)])
    medium_inode = _tsk_inode_for_path(
        sleuthkit_bin, image, partition_start, medium_path)
    medium_depth, medium_extents = _ext4_extent_tree_stats(
        image, partition_start, medium_inode)
    assert medium_depth == 1, (
        f"Expected a depth-1 tree, got depth={medium_depth}, "
        f"extents={medium_extents}")
    assert 4 < medium_extents <= (4096 - 12) // 12
    assert_clean_filesystem()
    apply_batch([("delete", medium_path)])
    assert_clean_filesystem()

    large_payload = Path(payload_value)
    large_path = "EXT4FRAG/LARGE.BIN"
    apply_batch([("create", large_payload, large_path)])
    inode_number = _tsk_inode_for_path(
        sleuthkit_bin, image, partition_start, large_path)
    extent_depth, extent_count = _ext4_extent_tree_stats(
        image, partition_start, inode_number)
    assert extent_depth == 2, f"Extent tree depth={extent_depth}, extents={extent_count}"
    assert extent_count > 4 * ((4096 - 12) // 12), extent_count
    assert_clean_filesystem()

    apply_batch([("replace", one_block, large_path)])
    inode_number = _tsk_inode_for_path(
        sleuthkit_bin, image, partition_start, large_path)
    extent_depth, extent_count = _ext4_extent_tree_stats(
        image, partition_start, inode_number)
    assert (extent_depth, extent_count) == (0, 1)
    assert_clean_filesystem()
    after_replace = _tsk_recover_ext(
        sleuthkit_bin, image, partition_start, tmp_path / "ext4-multilevel-replaced")
    assert all(after_replace.get(path) == digest for path, digest in before.items())
    recovered = next(path for path in after_replace
                     if _fat_path_matches(path, large_path))
    assert after_replace[recovered] == _sha256_file(one_block)

    apply_batch([("delete", large_path)])
    assert_clean_filesystem()
    after_delete = _tsk_recover_ext(
        sleuthkit_bin, image, partition_start, tmp_path / "ext4-multilevel-deleted")
    assert all(after_delete.get(path) == digest for path, digest in before.items())
    assert not any(_fat_path_matches(path, large_path) for path in after_delete)
    assert _sha256_file(sample) == source_hash


def test_chr_7207_ext4_enospc_add_aborts_without_image_changes(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to the extracted CHR image")

    sample = Path(sample_value)
    source_hash = _sha256_file(sample)
    image = tmp_path / sample.name
    image_hash = _copy_file_sha256(sample, image)
    payload = tmp_path / "ext4-enospc-payload.bin"
    block = b"P" * (1024 * 1024)
    with payload.open("wb") as output:
        for _ in range(96):
            output.write(block)

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    active = False
    try:
        target_index = _fat_writer_target_index(writer_lib, image, "Ext4")
        assert writer_lib.peare_writer_add_files_begin_target(
            writer, str(image).encode(), target_index) == 0, \
            writer_lib.peare_writer_last_error(writer)
        active = True
        status = writer_lib.peare_writer_add_file(
            writer, str(payload).encode(), b"PEARE-ENOSPC-96M.BIN")
        error = writer_lib.peare_writer_last_error(writer).decode("utf-8")
        assert status != 0, "Ext4 accepted a file larger than the volume's free space"
        assert "Ext could not write" in error, error
        writer_lib.peare_writer_abort(writer)
        active = False
    finally:
        if active:
            writer_lib.peare_writer_abort(writer)
        writer_lib.peare_writer_destroy(writer)

    assert _sha256_file(image) == image_hash
    assert _sha256_file(sample) == source_hash
    assert not Path(str(image) + ".peare-journal").exists()


@pytest.mark.parametrize(("qemu_format", "variant", "extension"), [
    ("vpc", "subformat=fixed", "vhd"),
    ("vpc", "subformat=dynamic", "vhd"),
    ("vhdx", "subformat=fixed", "vhdx"),
    ("vhdx", "subformat=dynamic", "vhdx"),
    ("vmdk", "subformat=monolithicSparse", "vmdk"),
    ("vmdk", "subformat=monolithicSparse,zeroed_grain=on", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentSparse", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentSparse,zeroed_grain=on", "vmdk"),
    ("vmdk", "subformat=monolithicFlat", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentFlat", "vmdk"),
    ("vmdk", "subformat=streamOptimized", "vmdk"),
])
def test_chr_7207_ext4_cud_through_virtual_disk_containers(
        writer_lib, peare_lib, tmp_path, qemu_format, variant, extension):
    sample = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to the extracted CHR image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")

    sample = Path(sample)
    original_hash = _sha256_file(sample)
    partition_start = 69632
    raw_copy = tmp_path / "chr-7207-source-copy.img"
    assert _copy_file_sha256(sample, raw_copy) == original_hash
    source_rows = _tsk_recover_ext(
        sleuthkit_bin, raw_copy, partition_start, tmp_path / "ext4-source")
    image = tmp_path / f"chr-7207-ext4-{qemu_format}-{variant.replace('=', '-')}.{extension}"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", qemu_format,
         "-o", variant, str(raw_copy), str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr

    target_index = _fat_writer_target_index(writer_lib, image, "Ext4")
    capabilities = C.c_uint32(0)
    assert writer_lib.peare_writer_get_update_capabilities(
        str(image).encode(), target_index, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f
    duplicate_name = next(iter(source_rows))
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, target_index, duplicate_name)
    cud = _fat_writer_cud(writer_lib, image, tmp_path, target_index=target_index)
    updated_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "EXT", max_depth=8)
    }
    for path, digest in source_rows.items():
        opener_path = next(
            (candidate for candidate in updated_rows
             if _fat_path_matches(candidate, path)), None)
        assert opener_path is not None, path
        assert updated_rows[opener_path] == digest.hex(), path
    final_path = next(path for path in updated_rows
                      if _fat_path_matches(path, cud["final_name"]))
    assert updated_rows[final_path] == hashlib.sha256(
        cud["replacement_payload"]).hexdigest()

    qemu_info = subprocess.run(
        [qemu_img, "info", "-f", qemu_format, "--output=json", str(image)],
        capture_output=True, text=True, timeout=60)
    assert qemu_info.returncode == 0, qemu_info.stdout + qemu_info.stderr
    assert json.loads(qemu_info.stdout)["format"] == qemu_format
    raw_result = tmp_path / f"ext4-roundtrip-{qemu_format}-{variant.replace('=', '-')}.img"
    roundtrip = subprocess.run(
        [qemu_img, "convert", "-f", qemu_format, "-O", "raw",
         str(image), str(raw_result)],
        capture_output=True, text=True, timeout=180)
    assert roundtrip.returncode == 0, roundtrip.stdout + roundtrip.stderr
    after = _tsk_recover_ext(
        sleuthkit_bin, raw_result, partition_start, tmp_path / "ext4-after")
    assert all(after.get(path) == digest for path, digest in source_rows.items())
    oracle_final = next(path for path in after
                        if _fat_path_matches(path, cud["final_name"]))
    assert after[oracle_final] == hashlib.sha256(
        cud["replacement_payload"]).digest()
    fsstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", str(partition_start), "-f", "ext", str(raw_result)],
        capture_output=True, text=True, timeout=60)
    assert fsstat.returncode == 0, fsstat.stdout + fsstat.stderr
    assert "File System Type: Ext4" in fsstat.stdout
    assert "Unmounted properly" in fsstat.stdout
    assert _sha256_file_range(
        raw_result, 2048 * 512, (partition_start - 2048) * 512) == \
        _sha256_file_range(raw_copy, 2048 * 512, (partition_start - 2048) * 512)
    assert _sha256_file(sample) == original_hash
    assert _sha256_file(raw_copy) == original_hash

@pytest.mark.parametrize(("variant", "create_type"), [
    ("subformat=twoGbMaxExtentSparse", "twoGbMaxExtentSparse"),
    ("subformat=twoGbMaxExtentSparse,zeroed_grain=on",
     "twoGbMaxExtentSparse"),
    ("subformat=twoGbMaxExtentFlat", "twoGbMaxExtentFlat"),
])
def test_chr_7207_ext4_cud_on_multi_extent_vmdk(
        writer_lib, peare_lib, tmp_path, variant, create_type):
    sample = os.environ.get("PEARE_CHR_7_20_7_LARGE_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_LARGE_TEST_IMAGE to the derived CHR image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")

    sample = Path(sample)
    original_hash = _sha256_file(sample)
    qemu_source_info = subprocess.run(
        [qemu_img, "info", "-f", "raw", "--output=json", str(sample)],
        capture_output=True, text=True, timeout=60)
    assert qemu_source_info.returncode == 0, (
        qemu_source_info.stdout + qemu_source_info.stderr)
    assert json.loads(qemu_source_info.stdout)["virtual-size"] > 2 * 1024**3

    partition_start = 69632
    source_rows = _tsk_recover_ext(
        sleuthkit_bin, sample, partition_start, tmp_path / "large-ext4-source")
    image = tmp_path / f"chr-7207-ext4-{create_type}.vmdk"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk",
         "-o", variant, str(sample), str(image)],
        capture_output=True, text=True, timeout=300)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    descriptor = image.read_text(encoding="latin1")
    assert f'createType="{create_type}"' in descriptor
    extent_lines = [
        line for line in descriptor.splitlines()
        if re.fullmatch(r'RW\s+\d+\s+(?:SPARSE|FLAT)\s+"[^"]+"(?:\s+\d+)?',
                        line.strip())
    ]
    assert len(extent_lines) >= 2, descriptor
    for line in extent_lines:
        extent_name = re.fullmatch(
            r'RW\s+\d+\s+(?:SPARSE|FLAT)\s+"([^"]+)"(?:\s+\d+)?',
            line.strip()).group(1)
        assert (image.parent / extent_name).is_file(), extent_name

    target_index = _fat_writer_target_index(writer_lib, image, "Ext4")
    capabilities = C.c_uint32(0)
    assert writer_lib.peare_writer_get_update_capabilities(
        str(image).encode(), target_index, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f
    duplicate_name = next(iter(source_rows))
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, target_index, duplicate_name)
    cud = _fat_writer_cud(writer_lib, image, tmp_path, target_index=target_index)

    raw_result = tmp_path / f"{create_type}-roundtrip.img"
    roundtrip = subprocess.run(
        [qemu_img, "convert", "-f", "vmdk", "-O", "raw",
         str(image), str(raw_result)],
        capture_output=True, text=True, timeout=300)
    assert roundtrip.returncode == 0, roundtrip.stdout + roundtrip.stderr
    after = _tsk_recover_ext(
        sleuthkit_bin, raw_result, partition_start, tmp_path / "large-ext4-after")
    assert all(after.get(path) == digest for path, digest in source_rows.items())
    oracle_final = next(path for path in after
                        if _fat_path_matches(path, cud["final_name"]))
    assert after[oracle_final] == hashlib.sha256(
        cud["replacement_payload"]).digest()
    fsstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b", "512",
         "-o", str(partition_start), "-f", "ext", str(raw_result)],
        capture_output=True, text=True, timeout=60)
    assert fsstat.returncode == 0, fsstat.stdout + fsstat.stderr
    assert "File System Type: Ext4" in fsstat.stdout
    assert "Unmounted properly" in fsstat.stdout
    assert _sha256_file_range(
        raw_result, 2048 * 512, (partition_start - 2048) * 512) == \
        _sha256_file_range(sample, 2048 * 512, (partition_start - 2048) * 512)
    assert _sha256_file(sample) == original_hash

def _disk_filesystem_tree(peare_lib, image, filesystem, max_depth=8):
    opener = peare_lib.open_file(str(image))
    assert opener is not None
    rows = []
    api = peare_lib._lib
    try:
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = po._HANDLE()
                if api.peare_opener_open_resource_at(
                        opener._h, folder, index, C.byref(resource)) != 0:
                    continue
                try:
                    context = po._Context()
                    if api.peare_resource_get_context(resource, C.byref(context)) != 0:
                        continue
                    name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                    is_filesystem = (bool(context.is_container) and
                                     po.CONTAINER_NAMES.get(context.container_format) == filesystem)
                    api.peare_resource_context_free(C.byref(context))
                    if not is_filesystem:
                        continue
                    source = po._HANDLE()
                    if api.peare_resource_get_source(resource, C.byref(source)) != 0:
                        continue
                    try:
                        child = peare_lib._open_source(source)
                        if child:
                            rows.extend(child.normalized_tree(name, max_depth=max_depth))
                            child.close()
                    finally:
                        api.peare_source_destroy(source)
                finally:
                    api.peare_resource_destroy(resource)
    finally:
        opener.close()
    return rows

def _fat_vmdk_tree(peare_lib, image):
    return _disk_filesystem_tree(peare_lib, image, "FAT")

def _sleuthkit_verify_vmdk_ntfs_payloads(sleuthkit_bin, image, payloads):
    bin_path = Path(sleuthkit_bin)
    mmls = subprocess.run(
        [str(bin_path / "mmls.exe"), "-i", "vmdk", str(image)],
        capture_output=True, text=True, timeout=60)
    assert mmls.returncode == 0, mmls.stdout + mmls.stderr
    offsets = re.findall(
        r"(?m)^\s*\d+:\s+\d+:\d+\s+(\d+)\s+\d+\s+\d+\s+NTFS\b",
        mmls.stdout)
    assert len(offsets) == 1, mmls.stdout
    offset = offsets[0]
    fsstat = subprocess.run(
        [str(bin_path / "fsstat.exe"), "-i", "vmdk", "-b", "512",
         "-o", offset, "-f", "ntfs", str(image)],
        capture_output=True, text=True, timeout=60)
    assert fsstat.returncode == 0, fsstat.stdout + fsstat.stderr
    assert "File System Type: NTFS" in fsstat.stdout
    fls = subprocess.run(
        [str(bin_path / "fls.exe"), "-i", "vmdk", "-b", "512", "-o",
         offset, "-f", "ntfs", "-r", "-p", str(image)],
        capture_output=True, text=True, timeout=120)
    assert fls.returncode == 0, fls.stdout + fls.stderr
    entries = {}
    for line in fls.stdout.splitlines():
        match = re.match(r"^r/r\s+([^:]+):\s+(.+)$", line)
        if match:
            entries[match.group(2).rsplit("/", 1)[-1].casefold()] = match.group(1)
    for name, expected in payloads:
        basename = name.replace("\\", "/").rsplit("/", 1)[-1]
        inode = entries.get(basename.casefold())
        assert inode, (name, fls.stdout)
        icat = subprocess.run(
            [str(bin_path / "icat.exe"), "-i", "vmdk", "-b", "512", "-o",
             offset, "-f", "ntfs", str(image), inode],
            capture_output=True, timeout=60)
        assert icat.returncode == 0, icat.stderr.decode(errors="replace")
        assert icat.stdout == expected, name

def _dsk_samples_from_env():
    configured = os.environ.get("PEARE_DSK_TEST_IMAGES", "")
    if not configured:
        configured = os.environ.get("PEARE_XDF_TEST_IMAGES", "")
    if not configured:
        configured = os.environ.get("PEARE_XDF_TEST_IMAGE", "")
    samples = [Path(value.strip()) for value in configured.split(";") if value.strip()]
    return samples or [None]

@pytest.mark.parametrize("sample", _ntfs_samples_from_env())
def test_ntfs_writer_cud_operations_on_disposable_sample_copy(
        writer_lib, peare_lib, tmp_path, sample):
    if sample is None:
        pytest.skip("Set PEARE_NTFS_TEST_IMAGES to local NTFS disk images")
    assert sample.is_file()
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    original_rows = _opener_root_snapshot(peare_lib, image)
    path = str(image).encode()
    targets = []
    for index in range(writer_lib.peare_writer_add_target_count(path)):
        name = C.create_string_buffer(256)
        assert writer_lib.peare_writer_get_add_target_name(
            path, index, name, len(name)) == 0
        targets.append(name.value.decode("utf-8"))
    target_indices = [index for index, name in enumerate(targets)
                      if " NTFS " in name]
    assert target_indices, targets
    original_ntfs_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=0)
    }
    assert original_ntfs_rows
    ntfs_tree = _disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=0)

    added_entries = []
    cud_entries = []
    oracle_payloads = []
    for target_index in target_indices:
        capabilities = C.c_uint32(0)
        assert writer_lib.peare_writer_get_update_capabilities(
            path, target_index, C.byref(capabilities)) == 0
        assert capabilities.value == 0x3f
        token = uuid.uuid4().hex[:10].upper()
        add_name = f"PEARE-NTFS-ADD-{token}.BIN"
        add_payload = f"NTFS Add Files ABI {token}".encode("ascii")
        add_source = tmp_path / f"ntfs-add-{token}.bin"
        add_source.write_bytes(add_payload)
        _fat_writer_add_target(
            writer_lib, image, add_source, add_name, target_index)
        target_match = re.search(r"partition\s+(\d+)", targets[target_index], re.I)
        assert target_match, targets[target_index]
        partition_prefix = f"Partition {target_match.group(1)} "
        directories = [
            row["path"] for row in ntfs_tree
            if row["is_container"] and row["path"].startswith(partition_prefix)
        ]
        assert directories, (targets[target_index], ntfs_tree)
        parent = next(
            (path.rsplit("/", 1)[-1] for path in directories
             if path.rsplit("/", 1)[-1].casefold() == "test"),
            directories[0].rsplit("/", 1)[-1])
        cud = _fat_writer_cud(
            writer_lib, image, tmp_path, target_index, parent)
        added_entries.append((add_name, hashlib.sha256(add_payload).hexdigest()))
        cud_entries.append(cud)
        oracle_payloads.extend([
            (add_name, add_payload),
            (cud["final_name"], cud["replacement_payload"]),
        ])

    updated_ntfs_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=1)
    }
    assert {path: updated_ntfs_rows[path] for path in original_ntfs_rows} == original_ntfs_rows
    for name, digest in added_entries:
        added_path = next(path for path in updated_ntfs_rows
                          if path.endswith("/" + name))
        assert updated_ntfs_rows[added_path] == digest
    for cud in cud_entries:
        final_path = next(path for path in updated_ntfs_rows
                          if path.endswith("/" + cud["final_name"]))
        assert updated_ntfs_rows[final_path] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
        assert not any(path.endswith("/" + name) for path in updated_ntfs_rows
                       for name in (cud["created_name"], cud["empty_name"],
                                    cud["directory_name"],
                                    cud["renamed_directory_name"]))

    assert _opener_root_snapshot(peare_lib, image) == original_rows
    assert _sha256_file(sample) == original_hash
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if (sample.name.casefold() == "intouch-validation.vmdk" and sleuthkit_bin and
            (Path(sleuthkit_bin) / "icat.exe").is_file()):
        _sleuthkit_verify_vmdk_ntfs_payloads(
            sleuthkit_bin, image, oracle_payloads)

    rollback_target = target_indices[0]
    rollback_before = _sha256_file(image)
    rollback_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=0)
    }
    rollback_name = f"PEARE-NTFS-ROLLBACK-{uuid.uuid4().hex[:10].upper()}.BIN"
    rollback_source = tmp_path / "ntfs-rollback.bin"
    rollback_source.write_bytes(b"staged write must not be published")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), rollback_target) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(rollback_source).encode(), rollback_name.encode()) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(rollback_source).encode(), rollback_name.encode()) != 0
        assert writer_lib.peare_writer_update_finish(writer) != 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == rollback_before
    assert {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=0)
    } == rollback_rows

    missing_name = f"PEARE-NTFS-MISSING-{uuid.uuid4().hex[:10].upper()}.BIN"
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), rollback_target) == 0
        assert writer_lib.peare_writer_replace_file(
            writer, str(rollback_source).encode(), missing_name.encode()) != 0
        assert writer_lib.peare_writer_update_finish(writer) != 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == rollback_before
    assert {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=0)
    } == rollback_rows

    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin_target(
            writer, str(image).encode(), rollback_target) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(rollback_source).encode(),
            b"../PEARE-NTFS-TRAVERSAL.BIN") != 0
        assert writer_lib.peare_writer_update_finish(writer) != 0
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _sha256_file(image) == rollback_before
    assert {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=0)
    } == rollback_rows

def _run_process_termination_recovery_test(
        writer_lib, tmp_path, sample, target_match, payload_size_mib,
        expect_file_growth=False):
    assert sample.is_file()
    image = tmp_path / sample.name
    original_hash = _copy_file_sha256(sample, image)
    image_path = str(image).encode()
    target_names = []
    for index in range(writer_lib.peare_writer_update_target_count(image_path)):
        name = C.create_string_buffer(256)
        assert writer_lib.peare_writer_get_update_target_name(
            image_path, index, name, len(name)) == 0
        target_names.append(name.value.decode("utf-8"))
    target_indices = [index for index, name in enumerate(target_names)
                      if target_match(name)]
    assert target_indices, target_names

    payload = tmp_path / "process-crash-payload.bin"
    block = bytes([0xA5]) * (1024 * 1024)
    with payload.open("wb") as output:
        for _ in range(payload_size_mib):
            output.write(block)

    entry_name = f"PEARE-CRASH-{uuid.uuid4().hex[:12].upper()}.BIN"
    child_script = """
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import test_writer
lib = test_writer.writer_lib.__wrapped__()
test_writer._fat_writer_add_target(
    lib, Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], int(sys.argv[5]))
"""
    original_size = image.stat().st_size
    child = subprocess.Popen(
        [sys.executable, "-c", child_script, str(Path(__file__).parent),
         str(image), str(payload), entry_name, str(target_indices[0])],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    journal = Path(str(image) + ".peare-journal")
    deadline = time.monotonic() + 180
    terminated_during_commit = False
    try:
        while child.poll() is None and time.monotonic() < deadline:
            if journal.is_file():
                with journal.open("rb") as source:
                    source.seek(8)
                    journal_state = source.read(4)
                if journal_state == b"\0\0\0\0":
                    child.kill()
                    child.wait(timeout=10)
                    terminated_during_commit = True
                    break
            time.sleep(0.001)
    finally:
        if child.poll() is None:
            child.kill()
            child.wait(timeout=10)

    stdout, stderr = child.communicate()
    assert terminated_during_commit, (
        "Did not observe the rollback-required journal while the disk image was being committed. "
        f"child exit={child.returncode}, stdout={stdout!r}, stderr={stderr!r}")
    assert journal.is_file()
    assert _sha256_file(image) != original_hash
    if expect_file_growth:
        assert image.stat().st_size > original_size

    assert writer_lib.peare_writer_update_target_count(image_path) > 0
    assert not journal.exists()
    assert image.stat().st_size == original_size
    assert _sha256_file(image) == original_hash
    assert _sha256_file(sample) == original_hash


def test_ntfs_vmdk_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the long process-crash test")
    sample_value = os.environ.get("PEARE_NTFS_CRASH_TEST_IMAGE", "")
    if not sample_value:
        pytest.skip("Set PEARE_NTFS_CRASH_TEST_IMAGE to a real NTFS VMDK sample")
    sample = Path(sample_value)
    assert sample.suffix.casefold() == ".vmdk"
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, sample, lambda name: " NTFS " in name, 64)


def test_ntfs_vhdx_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the long process-crash test")
    sample_value = os.environ.get("PEARE_VHDX_CRASH_TEST_IMAGE", "")
    if not sample_value:
        pytest.skip("Set PEARE_VHDX_CRASH_TEST_IMAGE to a real NTFS VHDX sample")
    sample = Path(sample_value)
    assert sample.suffix.casefold() == ".vhdx"
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, sample, lambda name: " NTFS " in name, 64)


@pytest.mark.parametrize("filesystem", ["Ext2", "Ext3"])
def test_vmdk_v2_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path, filesystem):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the long process-crash test")
    sample_value = os.environ.get("PEARE_EXT_DISK_TEST_IMAGE", "")
    qemu_img = os.environ.get("PEARE_QEMU_IMG", "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_EXT_DISK_TEST_IMAGE to the real CHR disk image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")

    source_hash = _sha256_file(sample_value)
    source_dir = tmp_path / "vmdk-v2-source"
    source_dir.mkdir()
    image = source_dir / "chr-zeroed-grain-v2.vmdk"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk",
         "-o", "subformat=monolithicSparse,zeroed_grain=on",
         sample_value, str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    with image.open("rb") as source:
        header = source.read(8)
    assert header[:4] == b"KDMV"
    assert int.from_bytes(header[4:8], "little") == 2
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, image,
        lambda name: f" {filesystem} " in name, 1)
    assert _sha256_file(sample_value) == source_hash

def test_ext_raw_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the process-crash test")
    sample_value = os.environ.get("PEARE_EXT_RAW_CRASH_TEST_IMAGE", "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_EXT_RAW_CRASH_TEST_IMAGE to a real Ext2 image")
    sample = Path(sample_value)
    assert sample.suffix.casefold() == ".img"
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, sample, lambda name: "Ext2" in name, 16)


def test_ext4_vmdk_v2_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the long process-crash test")
    sample_value = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE", "")
    qemu_img = os.environ.get("PEARE_QEMU_IMG", "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to the extracted CHR image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")

    source_hash = _sha256_file(sample_value)
    source_dir = tmp_path / "ext4-vmdk-v2-source"
    source_dir.mkdir()
    image = source_dir / "chr-7207-ext4-zeroed-grain-v2.vmdk"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk",
         "-o", "subformat=monolithicSparse,zeroed_grain=on",
         sample_value, str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    with image.open("rb") as source:
        header = source.read(8)
    assert header[:4] == b"KDMV"
    assert int.from_bytes(header[4:8], "little") == 2
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, image, lambda name: "Ext4" in name, 32)
    assert _sha256_file(sample_value) == source_hash


def test_exfat_vmdk_v2_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the long process-crash test")
    sample_value = os.environ.get("PEARE_EXFAT_TEST_IMAGE", "")
    qemu_img = os.environ.get("PEARE_QEMU_IMG", "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_EXFAT_TEST_IMAGE to an exFAT sample")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")

    source_hash = _sha256_file(sample_value)
    source_dir = tmp_path / "exfat-vmdk-v2-source"
    source_dir.mkdir()
    image = source_dir / "exfat-zeroed-grain-v2.vmdk"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk",
         "-o", "subformat=monolithicSparse,zeroed_grain=on",
         sample_value, str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    with image.open("rb") as source:
        header = source.read(8)
    assert header[:4] == b"KDMV"
    assert int.from_bytes(header[4:8], "little") == 2
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, image, lambda name: "exFAT" in name, 32)
    assert _sha256_file(sample_value) == source_hash


@pytest.mark.parametrize("filesystem", ["FAT", "NTFS"])
@pytest.mark.parametrize("crash_phase", ["staging", "published"])
def test_split_vmdk_process_termination_cleans_unpublished_files(
        writer_lib, peare_lib, tmp_path, crash_phase, filesystem):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the long process-crash test")
    sample_value = os.environ.get("PEARE_SPLIT_VMDK_CRASH_TEST_IMAGE", "")
    if not sample_value:
        pytest.skip("Set PEARE_SPLIT_VMDK_CRASH_TEST_IMAGE to a split VMDK sample")

    source_descriptor = Path(sample_value).resolve()
    descriptor_bytes = source_descriptor.read_bytes()
    descriptor_text = descriptor_bytes.decode("latin1")
    assert 'createType="twoGbMaxExtentSparse"' in descriptor_text
    extent_names = []
    for line in descriptor_text.splitlines():
        if not line.startswith("RW "):
            continue
        match = re.fullmatch(r'RW\s+\d+\s+SPARSE\s+"([^"]+)"\s*', line)
        assert match is not None
        name = match.group(1)
        assert Path(name).name == name and name not in (".", "..")
        extent_names.append(name)
    assert len(extent_names) > 1

    copy_dir = tmp_path / "split-vmdk-crash-copy"
    copy_dir.mkdir()
    image = copy_dir / source_descriptor.name
    source_hashes = {}
    copy_extent_hashes = {}
    try:
        shutil.copy2(source_descriptor, image)
        for name in extent_names:
            source_extent = source_descriptor.parent / name
            copy_extent = copy_dir / name
            source_hashes[source_extent] = _copy_file_sha256(
                source_extent, copy_extent)
            copy_extent_hashes[copy_extent] = source_hashes[source_extent]

        target_names = _fat_writer_targets(writer_lib, image)
        matching_targets = [index for index, name in enumerate(target_names)
                            if f" {filesystem} " in name]
        assert matching_targets, target_names
        target_index = matching_targets[-1] if filesystem == "NTFS" else matching_targets[0]

        def snapshot_tree(kind):
            rows = (_disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=0)
                    if kind == "NTFS" else _fat_vmdk_tree(peare_lib, image))
            return {row["path"]: row["sha256"] for row in rows}

        original_fat_rows = snapshot_tree("FAT")
        original_ntfs_rows = snapshot_tree("NTFS")
        original_rows = (original_ntfs_rows if filesystem == "NTFS"
                         else original_fat_rows)
        assert original_rows

        payload = tmp_path / "split-vmdk-crash-payload.bin"
        block = bytes([0x5A]) * (1024 * 1024)
        with payload.open("wb") as output:
            payload_mib = 4 if filesystem == "FAT" else 128
            for _ in range(payload_mib):
                output.write(block)
        entry_name = f"PEARE-CRASH-{uuid.uuid4().hex[:12].upper()}.BIN"
        child_script = """
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import test_writer
lib = test_writer.writer_lib.__wrapped__()
test_writer._fat_writer_add_target(
    lib, Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], int(sys.argv[5]))
"""
        child = subprocess.Popen(
            [sys.executable, "-c", child_script, str(Path(__file__).parent),
             str(image), str(payload), entry_name, str(target_index)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 180
        terminated_in_phase = False
        staged_pattern = ".peare-extent-*"
        published_pattern = "peare-*.vmdk"
        try:
            while child.poll() is None and time.monotonic() < deadline:
                pattern = staged_pattern if crash_phase == "staging" else published_pattern
                prepared = list(copy_dir.glob(pattern))
                if prepared and image.read_bytes() == descriptor_bytes:
                    child.kill()
                    child.wait(timeout=10)
                    terminated_in_phase = True
                    break
                time.sleep(0.001)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)

        stdout, stderr = child.communicate()
        assert terminated_in_phase, (
            f"Did not stop the Writer during the {crash_phase} phase. "
            f"child exit={child.returncode}, stdout={stdout!r}, stderr={stderr!r}")
        assert image.read_bytes() == descriptor_bytes
        staged_orphans = list(copy_dir.glob(staged_pattern))
        published_orphans = list(copy_dir.glob(published_pattern))
        if crash_phase == "staging":
            assert staged_orphans
            assert not published_orphans
        else:
            assert len(published_orphans) == 1
        assert writer_lib.peare_writer_add_target_count(str(image).encode()) > 0
        assert snapshot_tree("FAT") == original_fat_rows
        assert snapshot_tree("NTFS") == original_ntfs_rows
        assert {path: _sha256_file(path) for path in copy_extent_hashes} == copy_extent_hashes

        followup_payload = tmp_path / "split-vmdk-recovery-payload.bin"
        followup_payload.write_bytes(b"post-crash split VMDK update")
        followup_name = f"PEARE-RECOVERED-{uuid.uuid4().hex[:10].upper()}.BIN"
        _fat_writer_add_target(
            writer_lib, image, followup_payload, followup_name, target_index)
        assert all(not orphan.exists()
                   for orphan in staged_orphans + published_orphans)
        updated_descriptor = image.read_bytes().decode("latin1")
        referenced_names = set()
        for line in updated_descriptor.splitlines():
            if not line.startswith("RW "):
                continue
            match = re.fullmatch(r'RW\s+\d+\s+SPARSE\s+"([^"]+)"\s*', line)
            assert match is not None
            referenced_names.add(Path(match.group(1)).name)
        published = {path.name for path in copy_dir.glob("peare-*.vmdk")}
        assert published and published <= referenced_names
        updated_fat_rows = snapshot_tree("FAT")
        updated_ntfs_rows = snapshot_tree("NTFS")
        assert {path: updated_fat_rows[path]
                for path in original_fat_rows} == original_fat_rows
        assert {path: updated_ntfs_rows[path]
                for path in original_ntfs_rows} == original_ntfs_rows
        updated_rows = (updated_ntfs_rows if filesystem == "NTFS"
                        else updated_fat_rows)
        added_path = next(path for path in updated_rows
                          if path.endswith("/" + followup_name))
        assert updated_rows[added_path] == hashlib.sha256(
            followup_payload.read_bytes()).hexdigest()
        assert {path: _sha256_file(path) for path in copy_extent_hashes} == copy_extent_hashes

        qemu_img = os.environ.get("PEARE_QEMU_IMG")
        if qemu_img and Path(qemu_img).is_file():
            check = subprocess.run(
                [qemu_img, "info", "--output=json", "-f", "vmdk", str(image)],
                capture_output=True, text=True, timeout=60)
            assert check.returncode == 0, check.stdout + check.stderr
            qemu_info = json.loads(check.stdout)
            qemu_data = qemu_info["format-specific"]["data"]
            assert qemu_data["create-type"] == "twoGbMaxExtentSparse"
            assert len(qemu_data["extents"]) > 1

        assert source_descriptor.read_bytes() == descriptor_bytes
        assert {path: _sha256_file(path) for path in source_hashes} == source_hashes
    finally:
        shutil.rmtree(copy_dir, ignore_errors=True)


def test_fat_vhd_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the long process-crash test")
    sample_value = os.environ.get("PEARE_FAT_CRASH_TEST_IMAGE", "")
    if not sample_value:
        pytest.skip("Set PEARE_FAT_CRASH_TEST_IMAGE to a real FAT VHD sample")
    sample = Path(sample_value)
    assert sample.suffix.casefold() == ".vhd"
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, sample, lambda name: " FAT " in name, 64, True)

def test_fat_vhdx_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the long process-crash test")
    sample_value = os.environ.get("PEARE_FAT_VHDX_CRASH_TEST_IMAGE", "")
    if not sample_value:
        pytest.skip("Set PEARE_FAT_VHDX_CRASH_TEST_IMAGE to a real-derived FAT VHDX sample")
    sample = Path(sample_value)
    assert sample.suffix.casefold() == ".vhdx"
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, sample, lambda name: " FAT " in name, 64, True)

@pytest.mark.parametrize(("sample_env", "filesystem"), [
    ("PEARE_EXFAT_4KN_DYNAMIC_TEST_IMAGE", "exFAT"),
    ("PEARE_EXFAT_4KN_FIXED_TEST_IMAGE", "exFAT"),
    ("PEARE_NTFS_4KN_DYNAMIC_TEST_IMAGE", "NTFS"),
    ("PEARE_NTFS_4KN_FIXED_TEST_IMAGE", "NTFS"),
])
def test_4kn_vhdx_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path, sample_env, filesystem):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the process-crash test")
    sample_value = os.environ.get(sample_env, "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip(f"Set {sample_env} to a validated 4Kn VHDX test image")
    sample = Path(sample_value)
    assert sample.suffix.casefold() == ".vhdx"
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, sample,
        lambda name: filesystem in name, 16)


def test_stream_optimized_vmdk_process_termination_recovers_disposable_copy(
        writer_lib, tmp_path):
    if os.environ.get("PEARE_RUN_PROCESS_CRASH_TEST") != "1":
        pytest.skip("Set PEARE_RUN_PROCESS_CRASH_TEST=1 to run the process-crash test")
    source_value = os.environ.get("PEARE_FAT_CRASH_TEST_IMAGE", "")
    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER", "")
    if not source_value or not Path(source_value).is_file():
        pytest.skip("Set PEARE_FAT_CRASH_TEST_IMAGE to the local DOS FAT VHD")
    if not vmware or not Path(vmware).is_file():
        pytest.skip("Set PEARE_VMWARE_VDISKMANAGER to VMware's disk manager")

    source = Path(source_value)
    source_hash = _sha256_file(source)
    sample_dir = tmp_path / "stream-optimized-source"
    sample_dir.mkdir()
    image = sample_dir / "dos-stream-optimized-crash.vmdk"
    conversion = subprocess.run(
        [vmware, "-r", str(source), "-t", "5", str(image)],
        capture_output=True, text=True, timeout=1800)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    sample_hash = _sha256_file(image)
    _run_process_termination_recovery_test(
        writer_lib, tmp_path, image, lambda name: True, 1)
    assert _sha256_file(image) == sample_hash
    assert _sha256_file(source) == source_hash




def test_stream_optimized_vmdk_real_sample_remains_unchanged(
        writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VMDK_STREAM_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VMDK_STREAM_TEST_IMAGE to a streamOptimized VMDK")

    source_hash = _sha256_file(sample)
    image = tmp_path / "stream-optimized-copy.vmdk"
    shutil.copyfile(sample, image)
    with image.open("rb") as stream:
        header = stream.read(512)
    assert header[:4] == b"KDMV"
    assert struct.unpack_from("<I", header, 4)[0] == 3
    assert struct.unpack_from("<H", header, 77)[0] == 1

    opener = peare_lib.open_file(str(image))
    assert opener is not None
    try:
        assert opener.container_format() == "VMDK"
        original_rows = {
            row["path"]: row["sha256"] for row in opener.normalized_tree()
        }
        assert original_rows
    finally:
        opener.close()

    copy_hash = _sha256_file(image)
    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 0
    assert _sha256_file(image) == copy_hash
    assert _sha256_file(sample) == source_hash

    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if vmware and Path(vmware).is_file():
        converted = tmp_path / "stream-optimized-vmware-converted.vmdk"
        conversion = subprocess.run(
            [vmware, "-r", str(image), "-t", "0", str(converted)],
            capture_output=True, text=True, timeout=1800)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        oracle = peare_lib.open_file(str(converted))
        assert oracle is not None
        try:
            oracle_rows = {
                row["path"]: row["sha256"] for row in oracle.normalized_tree()
            }
            assert oracle_rows == original_rows
        finally:
            oracle.close()

def test_stream_optimized_vmdk_write_cud_vmware_sample(
        writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VMDK_TEST_IMAGE")
    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VMDK_TEST_IMAGE to the real DOS mTCP VMDK")
    if not vmware or not Path(vmware).is_file():
        pytest.skip("Set PEARE_VMWARE_VDISKMANAGER to VMware's disk manager")

    source = Path(sample)
    source_hash = _sha256_file(source)
    image = tmp_path / "mTCP-stream-optimized.vmdk"
    conversion = subprocess.run(
        [vmware, "-r", str(source), "-t", "5", str(image)],
        capture_output=True, text=True, timeout=1800)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    assert len(_fat_writer_targets(writer_lib, image)) == 1
    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1

    original = peare_lib.open_file(str(image))
    assert original is not None
    try:
        original_rows = {
            row["path"]: row["sha256"] for row in original.normalized_tree()
        }
    finally:
        original.close()

    payload = ROOT / "COPYRIGHT.md"
    _fat_writer_add_target(
        writer_lib, image, payload, "PEARE-COPYRIGHT.MD", 0)
    cud = _fat_writer_cud(writer_lib, image, tmp_path, 0)
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, cud["final_name"])

    updated = peare_lib.open_file(str(image))
    assert updated is not None
    try:
        rows = {row["path"]: row["sha256"] for row in updated.normalized_tree()}
        added = next(path for path in rows
                     if path.endswith("/PEARE-COPYRIGHT.MD"))
        assert rows[added] == hashlib.sha256(payload.read_bytes()).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"]))
        assert rows[final] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        updated.close()

    check = subprocess.run(
        [vmware, "-R", str(image)], capture_output=True, text=True, timeout=900)
    assert check.returncode == 0, check.stdout + check.stderr
    converted = tmp_path / "vmware-converted.vmdk"
    conversion = subprocess.run(
        [vmware, "-r", str(image), "-t", "0", str(converted)],
        capture_output=True, text=True, timeout=1800)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    oracle = peare_lib.open_file(str(converted))
    assert oracle is not None
    try:
        oracle_rows = {
            row["path"]: row["sha256"] for row in oracle.normalized_tree()
        }
        assert oracle_rows == rows
    finally:
        oracle.close()
    assert _sha256_file(source) == source_hash


def test_stream_optimized_vmdk_rename_latency_from_local_vhd(
        writer_lib, peare_lib, tmp_path):
    source_value = os.environ.get("PEARE_FAT_CRASH_TEST_IMAGE", "")
    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER", "")
    if not source_value or not Path(source_value).is_file():
        pytest.skip("Set PEARE_FAT_CRASH_TEST_IMAGE to the local DOS FAT VHD")
    if not vmware or not Path(vmware).is_file():
        pytest.skip("Set PEARE_VMWARE_VDISKMANAGER to VMware's disk manager")

    source = Path(source_value)
    assert source.suffix.casefold() == ".vhd"
    source_hash = _sha256_file(source)
    sample = tmp_path / "dos-stream-optimized.vmdk"
    conversion = subprocess.run(
        [vmware, "-r", str(source), "-t", "5", str(sample)],
        capture_output=True, text=True, timeout=1800)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr

    original = peare_lib.open_file(str(sample))
    assert original is not None
    try:
        original_rows = {
            row["path"]: row["sha256"] for row in original.normalized_tree()
        }
    finally:
        original.close()
    payload = ROOT / "README.md"
    payload_hash = _sha256_file(payload).hex()
    measurements = []

    for run in range(3):
        image = tmp_path / f"stream-optimized-{run}.vmdk"
        shutil.copyfile(sample, image)
        created_name = f"PEA{run:04d}.TXT"
        renamed_name = f"PEA{run:04d}.REN"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), 0)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started

            started = time.perf_counter()
            status = writer_lib.peare_writer_create_file(
                writer, str(payload).encode(), created_name.encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            create_seconds = time.perf_counter() - started

            started = time.perf_counter()
            status = writer_lib.peare_writer_rename_entry(
                writer, created_name.encode(), renamed_name.encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started

            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            measurements.append((begin_seconds, create_seconds, rename_seconds,
                                 finish_seconds))
        finally:
            writer_lib.peare_writer_destroy(writer)

        check = subprocess.run(
            [vmware, "-R", str(image)], capture_output=True, text=True,
            timeout=900)
        assert check.returncode == 0, check.stdout + check.stderr
        converted = tmp_path / f"stream-optimized-{run}-raw.vmdk"
        conversion = subprocess.run(
            [vmware, "-r", str(image), "-t", "0", str(converted)],
            capture_output=True, text=True, timeout=1800)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        updated = peare_lib.open_file(str(converted))
        assert updated is not None
        try:
            rows = {row["path"]: row["sha256"]
                    for row in updated.normalized_tree()}
            added = next(path for path in rows
                         if _fat_path_matches(path, renamed_name))
            assert rows[added] == payload_hash
            assert {path: rows[path] for path in original_rows} == original_rows
        finally:
            updated.close()

    print("VMDK streamOptimized rename seconds:", measurements)
    assert max(row[2] for row in measurements) < 10.0
    assert _sha256_file(source) == source_hash


def test_writer_refuses_ubuntu_vmdk_with_divergent_grain_tables(writer_lib):
    sample = os.environ.get("PEARE_UNSAFE_VMDK_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_UNSAFE_VMDK_TEST_IMAGE to the Ubuntu VMDK descriptor")
    descriptor = Path(sample)
    text = descriptor.read_text(encoding="utf-8", errors="replace")
    extent_names = re.findall(
        r'^(?:RW|RDONLY|NOACCESS)\s+\d+\s+\S+\s+"([^"]+)"',
        text, re.MULTILINE)
    assert extent_names
    files = [descriptor] + [descriptor.parent / name for name in extent_names]
    assert all(path.is_file() for path in files)
    before = [(path, path.stat().st_size, path.stat().st_mtime_ns)
              for path in files]

    assert writer_lib.peare_writer_can_add_files(str(descriptor).encode()) == 0
    assert _fat_writer_targets(writer_lib, descriptor) == []

    after = [(path, path.stat().st_size, path.stat().st_mtime_ns)
             for path in files]
    assert after == before

def test_mtcp_vmdk_sample_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VMDK_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VMDK_TEST_IMAGE to a disposable monolithic sparse VMDK sample")
    source_hash = _sha256_file(sample)
    image = tmp_path / "mTCP-VMDK-copy.vmdk"
    shutil.copyfile(sample, image)
    assert len(_fat_writer_targets(writer_lib, image)) == 1
    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = {row["path"]: row["sha256"] for row in before.normalized_tree()}
    finally:
        before.close()
    payload = ("Peare VMDK create validation " + uuid.uuid4().hex).encode("ascii")
    source = tmp_path / "VMDK validation payload.txt"
    source.write_bytes(payload)
    _fat_writer_add(writer_lib, image, source, "PEARE-VALIDATION.TXT")
    cud = _fat_writer_cud(writer_lib, image, tmp_path)

    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
        added = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"]))
        assert rows[added] == hashlib.sha256(cud["replacement_payload"]).hexdigest()
        added_files = [path for path in rows if path.endswith("/PEARE-VALIDATION.TXT")]
        assert len(added_files) == 1
        assert rows[added_files[0]] == hashlib.sha256(payload).hexdigest()
        assert not any(_fat_path_matches(path, name)
                       for path in rows for name in (
                           cud["created_name"], cud["empty_name"],
                           cud["directory_name"],
                           cud["renamed_directory_name"]))
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        after.close()

    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if vmware and Path(vmware).is_file():
        check = subprocess.run(
            [vmware, "-R", str(image)], capture_output=True, text=True,
            timeout=900)
        assert check.returncode == 0, check.stdout + check.stderr
        converted = tmp_path / "monolithic-vmdk-vmware-converted.vmdk"
        conversion = subprocess.run(
            [vmware, "-r", str(image), "-t", "0", str(converted)],
            capture_output=True, text=True, timeout=1800)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        converted_opener = peare_lib.open_file(str(converted))
        assert converted_opener is not None
        try:
            converted_rows = {
                row["path"]: row["sha256"]
                for row in converted_opener.normalized_tree()
            }
            converted_added = next(
                path for path in converted_rows
                if _fat_path_matches(path, cud["final_name"]))
            assert converted_rows[converted_added] == hashlib.sha256(
                cud["replacement_payload"]).hexdigest()
            converted_add_files = [
                path for path in converted_rows
                if path.endswith("/PEARE-VALIDATION.TXT")]
            assert len(converted_add_files) == 1
            assert converted_rows[converted_add_files[0]] == hashlib.sha256(payload).hexdigest()
            assert {path: converted_rows[path] for path in original_rows} == original_rows
        finally:
            converted_opener.close()
    assert _sha256_file(sample) == source_hash

def test_split_vmdk_efi_sample_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VMDK_SPLIT_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VMDK_SPLIT_TEST_IMAGE to a disposable split VMDK sample")

    source_descriptor = Path(sample).resolve()
    descriptor_bytes = source_descriptor.read_bytes()
    descriptor_text = descriptor_bytes.decode("latin1")
    assert 'createType="twoGbMaxExtentSparse"' in descriptor_text
    extent_names = []
    for line in descriptor_text.splitlines():
        if not line.startswith("RW "):
            continue
        match = re.fullmatch(r'RW\s+\d+\s+SPARSE\s+"([^"]+)"\s*', line)
        assert match is not None
        name = match.group(1)
        assert Path(name).name == name and name not in (".", "..")
        extent_names.append(name)
    assert len(extent_names) >= 2

    source_files = [source_descriptor.parent / name for name in extent_names]
    source_state = {
        path: (path.stat().st_size, path.stat().st_mtime_ns) for path in source_files
    }
    source_descriptor_hash = hashlib.sha256(descriptor_bytes).digest()
    copy_dir = tmp_path / "split-vmdk-efi-copy"
    copy_dir.mkdir()
    image = copy_dir / source_descriptor.name
    source_hashes = {}
    try:
        shutil.copy2(source_descriptor, image)
        for source_extent, name in zip(source_files, extent_names):
            source_hashes[source_extent] = _copy_file_sha256(
                source_extent, copy_dir / name)

        targets = _fat_writer_targets(writer_lib, image)
        fat_target_indices = [i for i, name in enumerate(targets)
                              if " FAT " in name]
        ntfs_target_indices = [i for i, name in enumerate(targets)
                               if " NTFS " in name]
        assert len(fat_target_indices) == 1, targets
        assert len(ntfs_target_indices) == 2, targets
        original_ntfs_rows = {
            row["path"]: row["sha256"]
            for row in _disk_filesystem_tree(peare_lib, image, "NTFS", max_depth=0)
        }
        assert original_ntfs_rows
        original_rows = {
            row["path"]: row["sha256"]
            for row in _fat_vmdk_tree(peare_lib, image)
        }
        assert original_rows

        payload = ("Peare split VMDK validation " + uuid.uuid4().hex).encode("ascii")
        payload_path = tmp_path / "split VMDK validation payload.txt"
        payload_path.write_bytes(payload)
        _fat_writer_add_target(writer_lib, image, payload_path,
                               "PEARE-VALIDATION.TXT", fat_target_indices[0])
        fat_cud = _fat_writer_cud(
            writer_lib, image, tmp_path, fat_target_indices[0])

        ntfs_additions = []
        ntfs_cud_results = []
        for target_index in ntfs_target_indices:
            capabilities = C.c_uint32(0)
            assert writer_lib.peare_writer_get_update_capabilities(
                str(image).encode(), target_index, C.byref(capabilities)) == 0
            assert capabilities.value == 0x3f
            token = uuid.uuid4().hex[:10].upper()
            add_name = f"PEARE-ADD-{token}.BIN"
            add_payload = f"split VMDK NTFS Add Files {token}".encode("ascii")
            add_source = tmp_path / f"split-vmdk-ntfs-add-{token}.bin"
            add_source.write_bytes(add_payload)
            _fat_writer_add_target(
                writer_lib, image, add_source, add_name, target_index)
            cud = _fat_writer_cud(writer_lib, image, tmp_path, target_index)
            ntfs_additions.append((add_name, hashlib.sha256(add_payload).hexdigest()))
            ntfs_cud_results.append(cud)

        rows = {
            row["path"]: row["sha256"]
            for row in _fat_vmdk_tree(peare_lib, image)
        }
        added = next(path for path in rows if path.endswith("/PEARE-VALIDATION.TXT"))
        assert rows[added] == hashlib.sha256(payload).hexdigest()
        fat_cud_path = next(path for path in rows
                            if _fat_path_matches(path, fat_cud["final_name"]))
        assert rows[fat_cud_path] == hashlib.sha256(
            fat_cud["replacement_payload"]).hexdigest()
        assert not any(_fat_path_matches(path, name)
                       for path in rows for name in (
                           fat_cud["created_name"], fat_cud["empty_name"],
                           fat_cud["directory_name"],
                           fat_cud["renamed_directory_name"]))
        assert {path: rows[path] for path in original_rows} == original_rows

        ntfs_rows = {
            row["path"]: row["sha256"]
            for row in _disk_filesystem_tree(
                peare_lib, image, "NTFS", max_depth=0)
        }
        assert {path: ntfs_rows[path] for path in original_ntfs_rows} == original_ntfs_rows
        for name, digest in ntfs_additions:
            added_path = next(path for path in ntfs_rows if path.endswith("/" + name))
            assert ntfs_rows[added_path] == digest
        for cud in ntfs_cud_results:
            final_path = next(path for path in ntfs_rows
                              if path.endswith("/" + cud["final_name"]))
            assert ntfs_rows[final_path] == hashlib.sha256(
                cud["replacement_payload"]).hexdigest()
            assert not any(path.endswith("/" + name) for path in ntfs_rows
                           for name in (cud["created_name"], cud["empty_name"],
                                        cud["directory_name"],
                                        cud["renamed_directory_name"]))

        vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
        if vmware and Path(vmware).is_file():
            check = subprocess.run(
                [vmware, "-R", str(image)], capture_output=True, text=True,
                timeout=900)
            assert check.returncode == 0, check.stdout + check.stderr
            converted = copy_dir / "vmware-converted.vmdk"
            conversion = subprocess.run(
                [vmware, "-r", str(image), "-t", "0", str(converted)],
                capture_output=True, text=True, timeout=1800)
            assert conversion.returncode == 0, conversion.stdout + conversion.stderr
            converted_rows = {
                row["path"]: row["sha256"]
                for row in _fat_vmdk_tree(peare_lib, converted)
            }
            converted_added = next(
                path for path in converted_rows
                if path.endswith("/PEARE-VALIDATION.TXT"))
            assert converted_rows[converted_added] == hashlib.sha256(payload).hexdigest()
            converted_cud = next(
                path for path in converted_rows
                if _fat_path_matches(path, fat_cud["final_name"]))
            assert converted_rows[converted_cud] == hashlib.sha256(
                fat_cud["replacement_payload"]).hexdigest()
            converted_ntfs_rows = {
                row["path"]: row["sha256"]
                for row in _disk_filesystem_tree(
                    peare_lib, converted, "NTFS", max_depth=0)
            }
            assert {path: converted_ntfs_rows[path]
                    for path in original_ntfs_rows} == original_ntfs_rows
            for name, digest in ntfs_additions:
                added_path = next(path for path in converted_ntfs_rows
                                  if path.endswith("/" + name))
                assert converted_ntfs_rows[added_path] == digest
            for cud in ntfs_cud_results:
                final_path = next(path for path in converted_ntfs_rows
                                  if path.endswith("/" + cud["final_name"]))
                assert converted_ntfs_rows[final_path] == hashlib.sha256(
                    cud["replacement_payload"]).hexdigest()

        assert source_descriptor.read_bytes() == descriptor_bytes
        assert hashlib.sha256(source_descriptor.read_bytes()).digest() == source_descriptor_hash
        assert {path: (path.stat().st_size, path.stat().st_mtime_ns)
                for path in source_files} == source_state
        assert {path: _sha256_file(path) for path in source_files} == source_hashes
    finally:
        shutil.rmtree(copy_dir, ignore_errors=True)

def test_dynamic_vhd_dos_sample_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VHD_DYNAMIC_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VHD_DYNAMIC_TEST_IMAGE to a disposable dynamic VHD sample")
    source_hash = _sha256_file(sample)
    image = tmp_path / "dynamic-vhd-sample-copy.vhd"
    shutil.copyfile(sample, image)
    targets = _fat_writer_targets(writer_lib, image)
    assert len(targets) == 1
    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = {row["path"]: row["sha256"] for row in before.normalized_tree()}
    finally:
        before.close()
    source = tmp_path / "DOS dynamic VHD validation.txt"
    payload = ("Peare dynamic VHD validation " + uuid.uuid4().hex).encode("ascii")
    source.write_bytes(payload)

    _fat_writer_add(writer_lib, image, source, "PEARE.TXT")
    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
        added = next(path for path in rows if path.endswith("/PEARE.TXT"))
        assert rows[added] == hashlib.sha256(payload).hexdigest()
        cud_path = next(path for path in rows
                        if _fat_path_matches(path, cud["final_name"]))
        assert rows[cud_path] == hashlib.sha256(cud["replacement_payload"]).hexdigest()
        assert not any(_fat_path_matches(path, name)
                       for path in rows for name in (
                           cud["created_name"], cud["empty_name"],
                           cud["directory_name"],
                           cud["renamed_directory_name"]))
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        after.close()

    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if vmware and Path(vmware).is_file():
        check = subprocess.run(
            [vmware, "-R", str(image)], capture_output=True, text=True,
            timeout=900)
        assert check.returncode == 0, check.stdout + check.stderr
        converted = tmp_path / "dynamic-vhd-vmware-converted.vmdk"
        conversion = subprocess.run(
            [vmware, "-r", str(image), "-t", "0", str(converted)],
            capture_output=True, text=True, timeout=900)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        converted_opener = peare_lib.open_file(str(converted))
        assert converted_opener is not None
        try:
            converted_rows = {
                row["path"]: row["sha256"]
                for row in converted_opener.normalized_tree()
            }
            converted_added = next(
                path for path in converted_rows if path.endswith("/PEARE.TXT"))
            assert converted_rows[converted_added] == hashlib.sha256(payload).hexdigest()
            converted_cud = next(
                path for path in converted_rows
                if _fat_path_matches(path, cud["final_name"]))
            assert converted_rows[converted_cud] == hashlib.sha256(
                cud["replacement_payload"]).hexdigest()
            assert {path: converted_rows[path] for path in original_rows} == original_rows
        finally:
            converted_opener.close()
    assert _sha256_file(sample) == source_hash

def test_peare_creates_monolithic_sparse_vmdk_with_cud_and_optional_vmware(
        writer_lib, peare_lib, tmp_path, filesystem_id):
    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")

    output = tmp_path / f"peare-created-{filesystem_id.decode()}.vmdk"
    source = tmp_path / "oracle-payload.txt"
    payload = b"Created by Peare and checked by VMware without conversion."
    source.write_bytes(payload)
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"ORACLE.TXT"))
    options = _WriterCreateOptions(b"vmdk", b"monolithic-sparse", filesystem_id)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    assert len(_fat_writer_targets(writer_lib, output)) == 1
    cud = _fat_writer_cud(writer_lib, output, tmp_path, 0)
    if filesystem_id != b"exfat":
        _assert_vmdk_filesystem_oracles(
            output, cud, payload, filesystem_id, tmp_path)
    opened = peare_lib.open_file(str(output))
    assert opened is not None
    try:
        rows = {row["path"].casefold(): row for row in opened.normalized_tree()}
        path = next(path for path in rows if path.endswith("/oracle.txt"))
        assert rows[path]["sha256"] == hashlib.sha256(payload).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"].casefold()))
        assert rows[final]["sha256"] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
    finally:
        opened.close()

    if vmware and Path(vmware).is_file():
        check = subprocess.run(
            [vmware, "-e", str(output)], capture_output=True,
            text=True, timeout=120)
        assert check.returncode == 0, check.stdout + check.stderr

def test_create_split_sparse_vmdk_with_cud_and_vmware_oracle(
        writer_lib, peare_lib, tmp_path, filesystem_id):
    variant_count = writer_lib.peare_writer_create_variant_count(b"vmdk")
    variants = (_WriterVariant * variant_count)()
    assert all(writer_lib.peare_writer_get_create_variant(
        b"vmdk", index, C.byref(variants[index])) == 0
        for index in range(variant_count))
    assert b"two-gb-sparse" in {variant.id for variant in variants}

    output = tmp_path / "peare-created-split.vmdk"
    source = tmp_path / "split-oracle-payload.txt"
    payload = b"Created as an external split sparse VMDK extent."
    source.write_bytes(payload)
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"ORACLE.TXT"))
    options = _WriterCreateOptions(b"vmdk", b"two-gb-sparse", filesystem_id)
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    descriptor = output.read_text(encoding="utf-8")
    assert 'createType="twoGbMaxExtentSparse"' in descriptor
    extent_name = next(
        line.split('"')[1] for line in descriptor.splitlines()
        if line.startswith("RW ") and " SPARSE " in line)
    assert (output.parent / extent_name).is_file()

    cud = _fat_writer_cud(writer_lib, output, tmp_path, 0)
    if filesystem_id != b"exfat":
        _assert_vmdk_filesystem_oracles(
            output, cud, payload, filesystem_id, tmp_path)

    opened = peare_lib.open_file(str(output))
    assert opened is not None
    try:
        rows = {row["path"].casefold(): row for row in opened.normalized_tree()}
        path = next(path for path in rows if path.endswith("/oracle.txt"))
        assert rows[path]["sha256"] == hashlib.sha256(payload).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"].casefold()))
        assert rows[final]["sha256"] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
    finally:
        opened.close()

    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if vmware and Path(vmware).is_file():
        check = subprocess.run(
            [vmware, "-e", str(output)], capture_output=True,
            text=True, timeout=120)
        assert check.returncode == 0, check.stdout + check.stderr

def test_create_stream_optimized_vmdk_with_cud_and_qemu_oracle(
        writer_lib, peare_lib, tmp_path):
    variants = (_WriterVariant * writer_lib.peare_writer_create_variant_count(
        b"vmdk"))()
    assert all(writer_lib.peare_writer_get_create_variant(
        b"vmdk", index, C.byref(variants[index])) == 0
        for index in range(len(variants)))
    assert b"stream-optimized" in {variant.id for variant in variants}

    output = tmp_path / "peare-created-stream-optimized.vmdk"
    source = tmp_path / "stream-optimized-payload.txt"
    payload = b"Created by Peare in streamOptimized VMDK format."
    source.write_bytes(payload)
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"ORACLE.TXT"))
    options = _WriterCreateOptions(b"vmdk", b"stream-optimized", b"fat12")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    header_descriptor = output.read_bytes()[512:512 + 20 * 512]
    assert b'createType="streamOptimized"' in header_descriptor
    cud = _fat_writer_cud(writer_lib, output, tmp_path, 0)
    opened = peare_lib.open_file(str(output))
    assert opened is not None
    try:
        rows = {row["path"].casefold(): row for row in opened.normalized_tree()}
        payload_path = next(path for path in rows if path.endswith("/oracle.txt"))
        assert rows[payload_path]["sha256"] == hashlib.sha256(payload).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"].casefold()))
        assert rows[final]["sha256"] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
    finally:
        opened.close()

    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if vmware and Path(vmware).is_file():
        check = subprocess.run(
            [vmware, "-e", str(output)], capture_output=True,
            text=True, timeout=120)
        assert check.returncode == 0, check.stdout + check.stderr

    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    if qemu_img and Path(qemu_img).is_file():
        raw = tmp_path / "stream-optimized-oracle.raw"
        check = subprocess.run(
            [qemu_img, "check", "-f", "vmdk", str(output)],
            capture_output=True, text=True, timeout=120)
        assert check.returncode == 0, check.stdout + check.stderr
        convert = subprocess.run(
            [qemu_img, "convert", "-f", "vmdk", "-O", "raw",
             str(output), str(raw)], capture_output=True, text=True, timeout=180)
        assert convert.returncode == 0, convert.stdout + convert.stderr
        assert raw.stat().st_size == 32 * 1024 * 1024

@pytest.mark.parametrize(("variant", "create_type"), [
    ("monolithic-flat", "monolithicFlat"),
    ("two-gb-flat", "twoGbMaxExtentFlat"),
])
def test_create_flat_vmdk_variants_with_full_cud_and_vmware(
        writer_lib, peare_lib, tmp_path, variant, create_type):
    variants = (_WriterVariant * writer_lib.peare_writer_create_variant_count(
        b"vmdk"))()
    assert all(writer_lib.peare_writer_get_create_variant(
        b"vmdk", index, C.byref(variants[index])) == 0
        for index in range(len(variants)))
    assert variant.encode() in {item.id for item in variants}

    output = tmp_path / f"peare-created-{variant}.vmdk"
    source = tmp_path / f"{variant}-payload.txt"
    payload = b"Created by Peare in flat VMDK format."
    source.write_bytes(payload)
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"ORACLE.TXT"))
    options = _WriterCreateOptions(b"vmdk", variant.encode(), b"fat12")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    descriptor = output.read_text(encoding="utf-8")
    assert f'createType="{create_type}"' in descriptor
    cud = _fat_writer_cud(writer_lib, output, tmp_path, 0)
    opened = peare_lib.open_file(str(output))
    assert opened is not None
    try:
        rows = {row["path"].casefold(): row for row in opened.normalized_tree()}
        payload_path = next(path for path in rows if path.endswith("/oracle.txt"))
        assert rows[payload_path]["sha256"] == hashlib.sha256(payload).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"].casefold()))
        assert rows[final]["sha256"] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
    finally:
        opened.close()

    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if vmware and Path(vmware).is_file():
        check = subprocess.run([vmware, "-e", str(output)], capture_output=True,
                               text=True, timeout=120)
        assert check.returncode == 0, check.stdout + check.stderr

@pytest.mark.parametrize("variant", ["monolithic-sparse", "two-gb-sparse"])
@pytest.mark.parametrize("view", ["tree", "details"])
def test_sparse_vmdk_create_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, variant, view):
    output = tmp_path / f"gui-created-{variant}-{view}.vmdk"
    source = tmp_path / f"gui-{variant}-{view}.txt"
    source.write_bytes(b"GUI sparse VMDK payload")
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"GUI.TXT"))
    options = _WriterCreateOptions(b"vmdk", variant.encode(), b"fat12")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    opened = peare_lib.open_file(str(output))
    assert opened is not None
    try:
        gui_root = next(row["path"] for row in opened.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        opened.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(output), view, gui_root)

@pytest.mark.parametrize("variant", ["monolithic-flat", "two-gb-flat"])
@pytest.mark.parametrize("view", ["tree", "details"])
def test_flat_vmdk_create_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, variant, view):
    output = tmp_path / f"gui-created-{variant}-{view}.vmdk"
    source = tmp_path / f"gui-{variant}-{view}.txt"
    source.write_bytes(b"GUI flat VMDK payload")
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"GUI.TXT"))
    options = _WriterCreateOptions(b"vmdk", variant.encode(), b"fat12")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    opened = peare_lib.open_file(str(output))
    assert opened is not None
    try:
        gui_root = next(row["path"] for row in opened.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        opened.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(output), view, gui_root)

@pytest.mark.parametrize("view", ["tree", "details"])
def test_stream_optimized_vmdk_create_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view):
    output = tmp_path / f"gui-created-stream-optimized-{view}.vmdk"
    source = tmp_path / f"gui-stream-optimized-{view}.txt"
    source.write_bytes(b"GUI streamOptimized payload")
    inputs = (_WriterInputFile * 1)((str(source).encode(), b"GUI.TXT"))
    options = _WriterCreateOptions(b"vmdk", b"stream-optimized", b"fat12")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    opened = peare_lib.open_file(str(output))
    assert opened is not None
    try:
        gui_root = next(row["path"] for row in opened.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        opened.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(output), view, gui_root)

def test_vmware_converted_dos_vhd_monolithic_vmdk_copy(
        writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VHD_DYNAMIC_TEST_IMAGE")
    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VHD_DYNAMIC_TEST_IMAGE to a DOS VHD sample")
    if not vmware or not Path(vmware).is_file():
        pytest.skip("Set PEARE_VMWARE_VDISKMANAGER to VMware vdiskmanager")

    source_hash = _sha256_file(sample)
    vhd_copy = tmp_path / "dos-vhd-source-copy.vhd"
    shutil.copyfile(sample, vhd_copy)
    image = tmp_path / "dos-monolithic-sparse.vmdk"
    conversion = subprocess.run(
        [vmware, "-r", str(vhd_copy), "-t", "0", str(image)],
        capture_output=True, text=True, timeout=900)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    assert len(_fat_writer_targets(writer_lib, image)) == 1

    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = {
            row["path"]: row["sha256"] for row in before.normalized_tree()
        }
    finally:
        before.close()

    payload = ("Peare VMware monolithic VMDK validation " + uuid.uuid4().hex).encode("ascii")
    source = tmp_path / "monolithic VMDK validation payload.txt"
    source.write_bytes(payload)
    _fat_writer_add(writer_lib, image, source, "PEARE-VALIDATION.TXT")

    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
        added = next(path for path in rows if path.endswith("/PEARE-VALIDATION.TXT"))
        assert rows[added] == hashlib.sha256(payload).hexdigest()
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        after.close()

    check = subprocess.run(
        [vmware, "-R", str(image)], capture_output=True, text=True, timeout=900)
    assert check.returncode == 0, check.stdout + check.stderr
    verified_image = tmp_path / "vmware-verified-monolithic.vmdk"
    verification = subprocess.run(
        [vmware, "-r", str(image), "-t", "0", str(verified_image)],
        capture_output=True, text=True, timeout=1800)
    assert verification.returncode == 0, verification.stdout + verification.stderr
    verified = peare_lib.open_file(str(verified_image))
    assert verified is not None
    try:
        verified_rows = {
            row["path"]: row["sha256"] for row in verified.normalized_tree()
        }
        verified_added = next(
            path for path in verified_rows
            if path.endswith("/PEARE-VALIDATION.TXT"))
        assert verified_rows[verified_added] == hashlib.sha256(payload).hexdigest()
        assert {path: verified_rows[path] for path in original_rows} == original_rows
    finally:
        verified.close()
    assert _sha256_file(sample) == source_hash

def _create_small_flat_vmdk_with_qemu(sample, qemu_img, image, subformat):
    source = Path(sample)
    assert source.stat().st_size <= 16 * 1024 * 1024, (
        "Flat VMDK tests require a source image of at most 16 MiB")
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk", "-o",
         f"subformat={subformat}", str(source), str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr

def test_qemu_converted_dos_floppy_monolithic_flat_vmdk_cud(
        writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to the real DOS FAT12 floppy")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not vmware or not Path(vmware).is_file():
        pytest.skip("Set PEARE_VMWARE_VDISKMANAGER to VMware vdiskmanager")

    source_hash = _sha256_file(sample)
    image = tmp_path / "dos-monolithic-flat.vmdk"
    _create_small_flat_vmdk_with_qemu(
        sample, qemu_img, image, "monolithicFlat")
    assert len(_fat_writer_targets(writer_lib, image)) == 1

    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = {
            row["path"]: row["sha256"] for row in before.normalized_tree()
        }
    finally:
        before.close()

    cud = _fat_writer_cud(writer_lib, image, tmp_path, 0)

    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"]))
        assert rows[final] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        after.close()

    check = subprocess.run(
        [vmware, "-R", str(image)], capture_output=True, text=True, timeout=900)
    assert check.returncode == 0, check.stdout + check.stderr
    verified_image = tmp_path / "vmware-verified-flat-converted.vmdk"
    verification = subprocess.run(
        [vmware, "-r", str(image), "-t", "0", str(verified_image)],
        capture_output=True, text=True, timeout=1800)
    assert verification.returncode == 0, verification.stdout + verification.stderr
    verified = peare_lib.open_file(str(verified_image))
    assert verified is not None
    try:
        verified_rows = {
            row["path"]: row["sha256"] for row in verified.normalized_tree()
        }
        verified_final = next(
            path for path in verified_rows
            if _fat_path_matches(path, cud["final_name"]))
        assert verified_rows[verified_final] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
        assert {path: verified_rows[path] for path in original_rows} == original_rows
    finally:
        verified.close()
    assert _sha256_file(sample) == source_hash

def test_qemu_converted_dos_floppy_two_gb_flat_vmdk_cud(
        writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to the real DOS FAT12 floppy")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not vmware or not Path(vmware).is_file():
        pytest.skip("Set PEARE_VMWARE_VDISKMANAGER to VMware vdiskmanager")

    source_hash = _sha256_file(sample)
    image = tmp_path / "dos-two-gb-flat.vmdk"
    _create_small_flat_vmdk_with_qemu(
        sample, qemu_img, image, "twoGbMaxExtentFlat")
    descriptor = image.read_text(encoding="utf-8")
    assert 'createType="twoGbMaxExtentFlat"' in descriptor
    assert len(_fat_writer_targets(writer_lib, image)) == 1

    original = peare_lib.open_file(str(image))
    assert original is not None
    try:
        original_rows = {
            row["path"]: row["sha256"] for row in original.normalized_tree()
        }
    finally:
        original.close()

    payload = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    _fat_writer_add_target(
        writer_lib, image, payload, "PEARE-SUPPORTED-FORMATS.MD", 0)
    cud = _fat_writer_cud(writer_lib, image, tmp_path, 0)
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, cud["final_name"])

    updated = peare_lib.open_file(str(image))
    assert updated is not None
    try:
        rows = {row["path"]: row["sha256"] for row in updated.normalized_tree()}
        added = next(path for path in rows
                     if path.endswith("/PEARE-SUPPORTED-FORMATS.MD"))
        assert rows[added] == hashlib.sha256(payload.read_bytes()).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"]))
        assert rows[final] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        updated.close()

    check = subprocess.run(
        [vmware, "-R", str(image)], capture_output=True, text=True, timeout=900)
    assert check.returncode == 0, check.stdout + check.stderr
    converted = tmp_path / "vmware-converted-two-gb-flat.vmdk"
    conversion = subprocess.run(
        [vmware, "-r", str(image), "-t", "0", str(converted)],
        capture_output=True, text=True, timeout=1800)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    oracle = peare_lib.open_file(str(converted))
    assert oracle is not None
    try:
        oracle_rows = {
            row["path"]: row["sha256"] for row in oracle.normalized_tree()
        }
        assert oracle_rows == rows
    finally:
        oracle.close()
    assert _sha256_file(sample) == source_hash

@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize("subformat", [
    "monolithicSparse", "twoGbMaxExtentSparse", "monolithicFlat",
    "twoGbMaxExtentFlat", "streamOptimized",
])
def test_vmdk_layout_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view, subformat):
    sample = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    vmware = os.environ.get("PEARE_VMWARE_VDISKMANAGER")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to the real DOS FAT12 floppy")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")

    source_hash = _sha256_file(sample)
    image = tmp_path / f"gui-{subformat}.vmdk"
    if subformat == "streamOptimized":
        if not vmware or not Path(vmware).is_file():
            pytest.skip("Set PEARE_VMWARE_VDISKMANAGER for streamOptimized")
        source_vhd = tmp_path / "gui-stream-source.vhd"
        conversion = subprocess.run(
            [qemu_img, "convert", "-f", "raw", "-O", "vpc", "-o",
             "subformat=dynamic", sample, str(source_vhd)],
            capture_output=True, text=True, timeout=180)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        conversion = subprocess.run(
            [vmware, "-r", str(source_vhd), "-t", "5", str(image)],
            capture_output=True, text=True, timeout=300)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    else:
        conversion = subprocess.run(
            [qemu_img, "convert", "-f", "raw", "-O", "vmdk", "-o",
             f"subformat={subformat}", sample, str(image)],
            capture_output=True, text=True, timeout=180)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    assert len(_fat_writer_targets(writer_lib, image)) == 1
    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        gui_root = next(row["path"] for row in before.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        before.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(image), view, gui_root)
    assert _sha256_file(sample) == source_hash

@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize("subformat", ["monolithicSparse", "twoGbMaxExtentSparse"])
def test_vmdk_sparse_v2_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view, subformat):
    sample = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to the real DOS FAT12 floppy")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")

    source_hash = _sha256_file(sample)
    image = tmp_path / f"gui-{subformat}-v2.vmdk"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk", "-o",
         f"subformat={subformat},zeroed_grain=on", sample, str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr

    if subformat == "monolithicSparse":
        sparse_extent = image
    else:
        descriptor = image.read_text(encoding="latin1")
        extent_name = next(
            line.strip().split('"')[1]
            for line in descriptor.splitlines()
            if re.fullmatch(r'RW\s+\d+\s+SPARSE\s+"[^"]+"', line.strip()))
        sparse_extent = image.parent / extent_name
    header = sparse_extent.read_bytes()[:8]
    assert header[:4] == b"KDMV"
    assert struct.unpack_from("<I", header, 4)[0] == 2

    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        gui_root = next(row["path"] for row in before.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        before.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(image), view, gui_root)
    assert _sha256_file(sample) == source_hash

@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize(("container", "image_format", "subformat"), [
    ("vhd-fixed", "vpc", "fixed"), ("vhd-dynamic", "vpc", "dynamic"),
    ("vhdx-fixed", "vhdx", "fixed"), ("vhdx-dynamic", "vhdx", "dynamic"),
])
def test_vhd_layout_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view, container,
        image_format, subformat):
    sample = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to the real DOS FAT12 floppy")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")

    source_hash = _sha256_file(sample)
    extension = "vhd" if image_format == "vpc" else "vhdx"
    image = tmp_path / f"gui-{container}.{extension}"
    conversion = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", image_format, "-o",
         f"subformat={subformat}", sample, str(image)],
        capture_output=True, text=True, timeout=180)
    assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    assert len(_fat_writer_targets(writer_lib, image)) == 1
    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        gui_root = next(row["path"] for row in before.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        before.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(image), view, gui_root)
    assert _sha256_file(sample) == source_hash

def test_exfat_writer_cud_external_sample(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_EXFAT_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXFAT_TEST_IMAGE to an exFAT image")

    source_hash = _sha256_file(sample)
    image = tmp_path / "exfat-cud-copy.img"
    shutil.copyfile(sample, image)
    targets = _fat_writer_targets(writer_lib, image)
    assert len(targets) == 1 and "exFAT" in targets[0], targets
    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1

    original = peare_lib.open_file(str(image))
    assert original is not None
    try:
        original_rows = {
            row["path"]: row["sha256"] for row in original.normalized_tree()
        }
    finally:
        original.close()

    payload = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    _fat_writer_add_target(
        writer_lib, image, payload, "PEARE-SUPPORTED-FORMATS.MD", 0)
    cud = _fat_writer_cud(writer_lib, image, tmp_path, 0)
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, cud["final_name"])

    updated = peare_lib.open_file(str(image))
    assert updated is not None
    try:
        rows = {row["path"]: row["sha256"] for row in updated.normalized_tree()}
        added = next(path for path in rows
                     if _fat_path_matches(path, "PEARE-SUPPORTED-FORMATS.MD"))
        assert rows[added] == hashlib.sha256(payload.read_bytes()).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"]))
        assert rows[final] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        updated.close()

    exfatprogs = os.environ.get("PEARE_EXFATPROGS")
    if exfatprogs and Path(exfatprogs).is_file():
        check = subprocess.run(
            [exfatprogs, "--unpin-program=fsck.exfat", "-n", str(image)],
            capture_output=True, text=True, timeout=120)
        assert check.returncode == 0, check.stdout + check.stderr
    assert _sha256_file(sample) == source_hash

@pytest.mark.parametrize(("qemu_format", "variant", "extension"), [
    ("vpc", "fixed", "vhd"),
    ("vpc", "dynamic", "vhd"),
    ("vhdx", "fixed", "vhdx"),
    ("vhdx", "dynamic", "vhdx"),
])
def test_qemu_virtual_disk_exfat_cud_variants(
        writer_lib, peare_lib, tmp_path, qemu_format, variant, extension):
    sample = os.environ.get("PEARE_EXFAT_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    exfatprogs = os.environ.get("PEARE_EXFATPROGS")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXFAT_TEST_IMAGE to an exFAT image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not exfatprogs or not Path(exfatprogs).is_file():
        pytest.skip("Set PEARE_EXFATPROGS to the independent exfatprogs tool")

    source_hash = _sha256_file(sample)
    image = tmp_path / f"exfat-{variant}.{extension}"
    format_options = f"subformat={variant}"
    if qemu_format == "vpc":
        format_options += ",force_size=on"
    created = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", qemu_format,
         "-o", format_options, sample, str(image)],
        capture_output=True, text=True, timeout=180)
    assert created.returncode == 0, created.stdout + created.stderr
    info = subprocess.run(
        [qemu_img, "info", "-f", qemu_format, "--output=json", str(image)],
        capture_output=True, text=True, timeout=60)
    assert info.returncode == 0, info.stdout + info.stderr
    assert json.loads(info.stdout)["format"] == qemu_format

    targets = _fat_writer_targets(writer_lib, image)
    assert len(targets) == 1, targets
    copy = tmp_path / f"exfat-{variant}-cud.{extension}"
    shutil.copyfile(image, copy)
    payload = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    _fat_writer_add_target(
        writer_lib, copy, payload, "PEARE-SUPPORTED-FORMATS.MD", 0)
    cud = _fat_writer_cud(writer_lib, copy, tmp_path, 0)
    _assert_duplicate_create_is_atomic(
        writer_lib, copy, [copy], tmp_path, 0, cud["final_name"])

    updated = peare_lib.open_file(str(copy))
    assert updated is not None
    try:
        rows = {row["path"]: row["sha256"] for row in updated.normalized_tree()}
        added = next(path for path in rows
                     if _fat_path_matches(path, "PEARE-SUPPORTED-FORMATS.MD"))
        assert rows[added] == hashlib.sha256(payload.read_bytes()).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"]))
        assert rows[final] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
    finally:
        updated.close()

    if qemu_format == "vhdx":
        image_check = subprocess.run(
            [qemu_img, "check", "-f", "vhdx", str(copy)],
            capture_output=True, text=True, timeout=120)
        assert image_check.returncode == 0, image_check.stdout + image_check.stderr

    raw_copy = tmp_path / f"exfat-{variant}-roundtrip.img"
    roundtrip = subprocess.run(
        [qemu_img, "convert", "-f", qemu_format, "-O", "raw",
         str(copy), str(raw_copy)], capture_output=True, text=True, timeout=180)
    assert roundtrip.returncode == 0, roundtrip.stdout + roundtrip.stderr
    check = subprocess.run(
        [exfatprogs, "--unpin-program=fsck.exfat", "-n", str(raw_copy)],
        capture_output=True, text=True, timeout=120)
    assert check.returncode == 0, check.stdout + check.stderr
    assert _sha256_file(sample) == source_hash

@pytest.mark.parametrize(("subformat", "header_version"), [
    ("monolithicSparse", 1), ("monolithicSparse", 2),
    ("twoGbMaxExtentSparse", 1), ("twoGbMaxExtentSparse", 2),
    ("monolithicFlat", None), ("twoGbMaxExtentFlat", None),
])
def test_qemu_vmdk_exfat_cud_variants(
        writer_lib, peare_lib, tmp_path, subformat, header_version):
    sample = os.environ.get("PEARE_EXFAT_TEST_IMAGE")
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    exfatprogs = os.environ.get("PEARE_EXFATPROGS")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXFAT_TEST_IMAGE to an exFAT image")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if not exfatprogs or not Path(exfatprogs).is_file():
        pytest.skip("Set PEARE_EXFATPROGS to the independent exfatprogs tool")

    source_hash = _sha256_file(sample)
    source_directory = tmp_path / "qemu-source"
    source_directory.mkdir()
    image = source_directory / "disk.vmdk"
    create_options = f"subformat={subformat}"
    if header_version is not None:
        zeroed_grain = "on" if header_version == 2 else "off"
        create_options += f",zeroed_grain={zeroed_grain}"
    created = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk", "-o",
         create_options, sample, str(image)],
        capture_output=True, text=True, timeout=180)
    assert created.returncode == 0, created.stdout + created.stderr
    extent = image
    extent_type = "SPARSE" if "Sparse" in subformat else "FLAT"
    if subformat != "monolithicSparse" and subformat != "monolithicFlat":
        descriptor = image.read_bytes().decode("utf-8", "replace")
        extent_pattern = (
            rf'^RW\s+\d+\s+{extent_type}\s+"([^"]+)"')
        extent_name = next(
            re.match(extent_pattern, line.strip()).group(1)
            for line in descriptor.splitlines()
            if re.match(extent_pattern, line.strip()))
        extent = source_directory / extent_name
    if header_version is None:
        descriptor = image.read_text(encoding="utf-8")
        assert f'createType="{subformat}"' in descriptor
    else:
        with extent.open("rb") as stream:
            stream.seek(4)
            assert struct.unpack("<I", stream.read(4))[0] == header_version
    assert len(_fat_writer_targets(writer_lib, image)) == 1

    copy_directory = tmp_path / "writer-copy"
    shutil.copytree(source_directory, copy_directory)
    copy = copy_directory / image.name
    payload = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    _fat_writer_add_target(
        writer_lib, copy, payload, "PEARE-SUPPORTED-FORMATS.MD", 0)
    cud = _fat_writer_cud(writer_lib, copy, tmp_path, 0)
    updated = peare_lib.open_file(str(copy))
    assert updated is not None
    try:
        rows = {row["path"]: row["sha256"] for row in updated.normalized_tree()}
        added = next(path for path in rows
                     if _fat_path_matches(path, "PEARE-SUPPORTED-FORMATS.MD"))
        assert rows[added] == hashlib.sha256(
            payload.read_bytes()).hexdigest()
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"]))
        assert rows[final] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
    finally:
        updated.close()

    _assert_duplicate_create_is_atomic(
        writer_lib, copy, list(copy_directory.iterdir()), tmp_path, 0,
        "PEARE-SUPPORTED-FORMATS.MD")
    version = f"-v{header_version}" if header_version is not None else ""
    raw_copy = tmp_path / f"exfat-vmdk-{subformat}{version}-roundtrip.img"
    roundtrip = subprocess.run(
        [qemu_img, "convert", "-f", "vmdk", "-O", "raw", str(copy),
         str(raw_copy)], capture_output=True, text=True, timeout=180)
    assert roundtrip.returncode == 0, roundtrip.stdout + roundtrip.stderr
    check = subprocess.run(
        [exfatprogs, "--unpin-program=fsck.exfat", "-n", str(raw_copy)],
        capture_output=True, text=True, timeout=120)
    assert check.returncode == 0, check.stdout + check.stderr
    assert _sha256_file(sample) == source_hash

@pytest.mark.parametrize(("format_id", "variant", "qemu_format", "extension", "sector_size"), [
    ("vhd", "fixed", "vpc", "vhd", 512),
    ("vhd", "dynamic", "vpc", "vhd", 512),
    ("vhdx", "fixed", "vhdx", "vhdx", 512),
    ("vhdx", "dynamic", "vhdx", "vhdx", 512),
    ("vhdx", "fixed-4kn", None, "vhdx", 4096),
    ("vhdx", "dynamic-4kn", None, "vhdx", 4096),
])
def test_peare_creates_vhd_and_vhdx_with_filesystem_oracles(
        writer_lib, peare_lib, tmp_path, format_id, variant,
        qemu_format, extension, sector_size, filesystem_id):
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    exfatprogs = os.environ.get("PEARE_EXFATPROGS")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    four_kn = sector_size == 4096
    qemu_available = qemu_img and Path(qemu_img).is_file() and not four_kn
    vmware_tool = Path(r"C:\Program Files (x86)\VMware\VMware Workstation\vmware-vdiskmanager.exe")
    filesystem = filesystem_id.decode()
    use_sleuthkit = filesystem != "exfat" and (
        qemu_available or format_id == "vhd" or four_kn)
    fsstat = Path(sleuthkit_bin or "") / "fsstat.exe"
    recover_tool = Path(sleuthkit_bin or "") / "tsk_recover.exe"
    use_sleuthkit = use_sleuthkit and fsstat.is_file() and recover_tool.is_file()
    if four_kn and not vmware_tool.is_file():
        pytest.skip("VMware Workstation is required to independently inspect 4Kn VHDX")

    output = tmp_path / f"peare-created-{format_id}-{variant}-{filesystem}.{extension}"
    source = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    options = _WriterCreateOptions(
        format_id.encode(), variant.encode(), filesystem_id)
    inputs = (_WriterInputFile * 1)(
        (str(source).encode(), b"SUPPORTED-FORMATS.MD"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    assert len(_fat_writer_targets(writer_lib, output)) == 1
    cud = _fat_writer_cud(writer_lib, output, tmp_path, 0)
    if four_kn:
        opener_filesystem = (
            "FAT" if filesystem.startswith("fat") else
            "EXFAT" if filesystem == "exfat" else "NTFS")
        opener_rows = _disk_filesystem_tree(
            peare_lib, output, opener_filesystem, max_depth=1)
        opener_files = {row["path"]: row["sha256"] for row in opener_rows}
        opener_source = next(
            path for path in opener_files
            if _fat_path_matches(path, "SUPPORTED-FORMATS.MD"))
        opener_final = next(
            path for path in opener_files
            if _fat_path_matches(path, cud["final_name"]))
        assert opener_files[opener_source] == hashlib.sha256(
            source.read_bytes()).hexdigest()
        assert opener_files[opener_final] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
    _assert_duplicate_create_is_atomic(
        writer_lib, output, [output], tmp_path, 0, "SUPPORTED-FORMATS.MD")
    if format_id == "vhdx":
        _assert_windows_vhdx_oracle(output, sector_size)

    if qemu_available:
        info = subprocess.run(
            [qemu_img, "info", "-f", qemu_format, "--output=json", str(output)],
            capture_output=True, text=True, timeout=60)
        assert info.returncode == 0, info.stdout + info.stderr
        assert json.loads(info.stdout)["format"] == qemu_format
    if qemu_available and qemu_format == "vhdx":
        check = subprocess.run(
            [qemu_img, "check", "-f", "vhdx", str(output)],
            capture_output=True, text=True, timeout=120)
        assert check.returncode == 0, check.stdout + check.stderr

    oracle_image = output
    oracle_type = format_id
    if qemu_available:
        oracle_image = tmp_path / f"peare-created-{format_id}-{variant}-{filesystem}.img"
        roundtrip = subprocess.run(
            [qemu_img, "convert", "-f", qemu_format, "-O", "raw",
             str(output), str(oracle_image)], capture_output=True, text=True,
            timeout=180)
        assert roundtrip.returncode == 0, roundtrip.stdout + roundtrip.stderr
        oracle_type = "raw"
    elif four_kn:
        converted = tmp_path / f"peare-created-{variant}-{filesystem}.vmdk"
        conversion = subprocess.run(
            [str(vmware_tool), "-r", str(output), "-t", "0", str(converted)],
            capture_output=True, text=True, timeout=300)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        if filesystem == "exfat":
            if (not qemu_img or not Path(qemu_img).is_file() or
                    not exfatprogs or not Path(exfatprogs).is_file()):
                return
            oracle_image = tmp_path / f"peare-created-{variant}-exfat.img"
            roundtrip = subprocess.run(
                [qemu_img, "convert", "-f", "vmdk", "-O", "raw",
                 str(converted), str(oracle_image)], capture_output=True,
                text=True, timeout=180)
            assert roundtrip.returncode == 0, roundtrip.stdout + roundtrip.stderr
            filesystem_check = subprocess.run(
                [exfatprogs, "--unpin-program=fsck.exfat", "-n",
                 str(oracle_image)], capture_output=True, text=True, timeout=120)
            assert filesystem_check.returncode == 0, (
                filesystem_check.stdout + filesystem_check.stderr)
        else:
            _assert_vmdk_filesystem_oracles(
                converted, cud, source.read_bytes(), filesystem_id, tmp_path,
                sector_size=sector_size, use_qemu=False,
                initial_name=b"SUPPORTED-FORMATS.MD")
        return
    elif format_id == "vhdx":
        return
    elif not use_sleuthkit:
        return
    if filesystem == "exfat":
        if not exfatprogs or not Path(exfatprogs).is_file():
            return
        filesystem_check = subprocess.run(
            [exfatprogs, "--unpin-program=fsck.exfat", "-n", str(oracle_image)],
            capture_output=True, text=True, timeout=120)
        assert filesystem_check.returncode == 0, (
            filesystem_check.stdout + filesystem_check.stderr)
    else:
        filesystem_check = subprocess.run(
            [str(fsstat), "-i", oracle_type, "-b", "512", "-f", filesystem,
             str(oracle_image)], capture_output=True, text=True, timeout=60)
        assert filesystem_check.returncode == 0, (
            filesystem_check.stdout + filesystem_check.stderr)
        assert f"File System Type: {filesystem.upper()}" in filesystem_check.stdout
        recovered = tmp_path / f"tsk-recovered-{format_id}-{variant}-{filesystem}"
        recovered.mkdir()
        extraction = subprocess.run(
            [str(recover_tool), "-i", oracle_type, "-b", "512", "-f", filesystem,
             "-e", str(oracle_image), str(recovered)],
            capture_output=True, text=True, timeout=120)
        assert extraction.returncode == 0, extraction.stdout + extraction.stderr
        recovered_source = next(recovered.rglob("SUPPORTED-FORMATS.MD"))
        recovered_final = next(recovered.rglob(cud["final_name"]))
        assert _sha256_file(recovered_source) == hashlib.sha256(
            source.read_bytes()).digest()
        assert _sha256_file(recovered_final) == hashlib.sha256(
            cud["replacement_payload"]).digest()

def test_vhdx_creation_variants_include_4kn(writer_lib):
    count = writer_lib.peare_writer_create_variant_count(b"vhdx")
    variants = (_WriterVariant * count)()
    assert all(writer_lib.peare_writer_get_create_variant(
        b"vhdx", index, C.byref(variants[index])) == 0
        for index in range(count))
    assert {variant.id for variant in variants} == {
        b"fixed", b"dynamic", b"fixed-4kn", b"dynamic-4kn"}

@pytest.mark.parametrize(("format_id", "variant"), [
    ("vhd", "fixed"), ("vhd", "dynamic"),
    ("vhdx", "fixed"), ("vhdx", "dynamic"),
])
@pytest.mark.parametrize("view", ["tree", "details"])
def test_vhd_vhdx_512_create_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path,
        format_id, variant, view):
    extension = "vhd" if format_id == "vhd" else "vhdx"
    image = tmp_path / f"gui-{format_id}-{variant}-{view}.{extension}"
    source = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    options = _WriterCreateOptions(
        format_id.encode(), variant.encode(), b"fat12")
    inputs = (_WriterInputFile * 1)(
        (str(source).encode(), b"SUPPORTED-FORMATS.MD"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(image).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    opened = peare_lib.open_file(str(image))
    assert opened is not None
    try:
        gui_root = next(row["path"] for row in opened.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        opened.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(image), view, gui_root)

@pytest.mark.parametrize("variant", ["fixed-4kn", "dynamic-4kn"])
@pytest.mark.parametrize("view", ["tree", "details"])
def test_vhdx_4kn_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, variant, view):
    vmware = Path(r"C:\Program Files (x86)\VMware\VMware Workstation\vmware-vdiskmanager.exe")
    if not vmware.is_file():
        pytest.skip("VMware Workstation is required for independent VHDX validation")
    image = tmp_path / f"vhdx-{variant}-fat12.vhdx"
    source = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    options = _WriterCreateOptions(b"vhdx", variant.encode(), b"fat12")
    inputs = (_WriterInputFile * 1)(
        (str(source).encode(), b"SUPPORTED-FORMATS.MD"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_to_ex(
            writer, str(image).encode(), C.byref(options), 0, inputs, 1) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        gui_root = next(row["path"] for row in before.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        before.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(image), view, gui_root)
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    gui_image = Path(report["image"])
    converted = tmp_path / f"vhdx-{variant}-{view}-vmware.vmdk"
    result = subprocess.run(
        [str(vmware), "-r", str(gui_image), "-t", "0", str(converted)],
        capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr
    opened = peare_lib.open_file(str(converted))
    assert opened is not None
    try:
        rows = {row["path"].casefold(): row["sha256"]
                for row in opened.normalized_tree()}
        expected = hashlib.sha256(source.read_bytes()).hexdigest()
        renamed = next(path for path in rows
                       if path.endswith("/peareuicud/renamed.txt"))
        assert rows[renamed] == expected
    finally:
        opened.close()

@pytest.mark.parametrize("partition_table", ["mbr", "gpt"])
def test_peare_creates_raw_partitioned_filesystem_with_sleuthkit(
        writer_lib, peare_lib, tmp_path, partition_table, filesystem_id):
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    mmls = Path(sleuthkit_bin or "") / "mmls.exe"
    recover_tool = Path(sleuthkit_bin or "") / "tsk_recover.exe"
    use_sleuthkit = mmls.is_file() and recover_tool.is_file()

    filesystem = filesystem_id.decode()
    image = tmp_path / f"peare-created-{partition_table}-{filesystem}.img"
    source = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    options = _WriterCreateOptions(
        b"raw-disk", partition_table.encode(), filesystem_id)
    inputs = (_WriterInputFile * 1)(
        (str(source).encode(), b"SUPPORTED-FORMATS.MD"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(image).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    assert len(_fat_writer_targets(writer_lib, image)) == 1
    cud = _fat_writer_cud(writer_lib, image, tmp_path, 0)
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, "SUPPORTED-FORMATS.MD")
    opened = peare_lib.open_file(str(image))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        added = next(path for path in rows
                     if _fat_path_matches(path, "SUPPORTED-FORMATS.MD"))
        final = next(path for path in rows
                     if _fat_path_matches(path, cud["final_name"]))
        assert rows[added] == hashlib.sha256(source.read_bytes()).hexdigest()
        assert rows[final] == hashlib.sha256(cud["replacement_payload"]).hexdigest()
    finally:
        opened.close()

    if not use_sleuthkit:
        return

    partition_listing = subprocess.run(
        [str(mmls), "-i", "raw", str(image)],
        capture_output=True, text=True, timeout=60)
    assert partition_listing.returncode == 0, (
        partition_listing.stdout + partition_listing.stderr)
    expected_label = (
        "GUID Partition Table" if partition_table == "gpt"
        else "DOS Partition Table")
    assert expected_label in partition_listing.stdout
    partition_rows = [
        re.match(r"^\s*\d+:\s+\d+(?::\d+)?\s+(\d+)\s+(\d+)\s+(\d+)", line)
        for line in partition_listing.stdout.splitlines()]
    partition_rows = [row for row in partition_rows if row]
    assert len(partition_rows) == 1, partition_listing.stdout
    partition_offset = partition_rows[0].group(1)

    recovered = tmp_path / f"tsk-recovered-{partition_table}"
    recovered.mkdir()
    extraction = subprocess.run(
        [str(recover_tool), "-i", "raw", "-b", "512", "-o", partition_offset,
         "-e", "-f", filesystem,
         str(image), str(recovered)],
        capture_output=True, text=True, timeout=120)
    assert extraction.returncode == 0, extraction.stdout + extraction.stderr
    payloads = list(recovered.rglob("SUPPORTED-FORMATS.MD"))
    replacements = list(recovered.rglob(cud["final_name"]))
    assert len(payloads) == len(replacements) == 1
    assert _sha256_file(payloads[0]) == hashlib.sha256(source.read_bytes()).digest()
    assert _sha256_file(replacements[0]) == hashlib.sha256(
        cud["replacement_payload"]).digest()

@pytest.mark.parametrize("view", ["tree", "details"])
def test_gpt_fat12_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view):
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    mmls = Path(sleuthkit_bin or "") / "mmls.exe"
    recover = Path(sleuthkit_bin or "") / "tsk_recover.exe"
    image = tmp_path / "gpt-fat12-gui.img"
    source = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    options = _WriterCreateOptions(b"raw-disk", b"gpt", b"fat12")
    inputs = (_WriterInputFile * 1)(
        (str(source).encode(), b"SUPPORTED-FORMATS.MD"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_to_ex(
            writer, str(image).encode(), C.byref(options), 0, inputs, 1) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    targets = _fat_writer_targets(writer_lib, image)
    target_index = next(index for index, target in enumerate(targets)
                        if "FAT" in target)
    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        gui_root = next(row["path"] for row in before.normalized_tree(max_depth=1)
                        if row["is_container"] and
                        row["path"].casefold().startswith("partition "))
    finally:
        before.close()

    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(image), view, gui_root, target_index=target_index,
                      tree_max_depth=2)
    if not mmls.is_file() or not recover.is_file():
        return
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    updated = Path(report["image"])
    partition_listing = subprocess.run(
        [str(mmls), "-i", "raw", str(updated)], capture_output=True,
        text=True, timeout=60)
    assert partition_listing.returncode == 0, (
        partition_listing.stdout + partition_listing.stderr)
    assert "GUID Partition Table" in partition_listing.stdout
    partition_rows = [
        re.match(r"^\s*\d+:\s+\d+(?::\d+)?\s+(\d+)\s+(\d+)\s+(\d+)", line)
        for line in partition_listing.stdout.splitlines()]
    partition_rows = [row for row in partition_rows if row]
    assert len(partition_rows) == 1, partition_listing.stdout
    recovered = tmp_path / "gpt-fat12-tsk-recovered"
    extraction = subprocess.run(
        [str(recover), "-i", "raw", "-b", "512", "-o",
         partition_rows[0].group(1), "-e", "-f", "fat", str(updated),
         str(recovered)], capture_output=True, text=True, timeout=120)
    assert extraction.returncode == 0, extraction.stdout + extraction.stderr
    renamed = list(recovered.rglob("renamed.txt"))
    assert len(renamed) == 1
    assert _sha256_file(renamed[0]) == hashlib.sha256(source.read_bytes()).digest()

@pytest.mark.parametrize("filesystem", ["ext2", "ext3", "ext4"])
@pytest.mark.parametrize(("format_id", "variant", "qemu_format", "extension",
                          "partition_offset"), [
    ("vhd", "fixed", "vpc", "vhd", 0),
    ("vhd", "dynamic", "vpc", "vhd", 0),
    ("vhdx", "fixed", "vhdx", "vhdx", 0),
    ("vhdx", "dynamic", "vhdx", "vhdx", 0),
    ("vhdx", "fixed-4kn", None, "vhdx", 0),
    ("vhdx", "dynamic-4kn", None, "vhdx", 0),
    ("vmdk", "monolithic-sparse", "vmdk", "vmdk", 0),
    ("vmdk", "two-gb-sparse", "vmdk", "vmdk", 0),
    ("raw-disk", "mbr", "raw", "img", 2048),
    ("raw-disk", "gpt", "raw", "img", 2048),
])
def test_peare_creates_ext_filesystems_with_independent_oracles(
        writer_lib, peare_lib, tmp_path, filesystem, format_id, variant,
        qemu_format, extension, partition_offset):
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    four_kn = variant.endswith("-4kn")
    vmware_tool = os.environ.get(
        "PEARE_VMWARE_VDISKMANAGER",
        r"C:\Program Files (x86)\VMware\VMware Workstation\vmware-vdiskmanager.exe")
    if format_id != "raw-disk" and (
            not qemu_img or not Path(qemu_img).is_file()):
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if four_kn and not Path(vmware_tool).is_file():
        pytest.skip("VMware Workstation is required to independently inspect 4Kn VHDX")
    if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
        pytest.skip("Set PEARE_SLEUTHKIT_BIN to The Sleuth Kit bin directory")

    output = tmp_path / f"peare-created-{filesystem}-{format_id}-{variant}.{extension}"
    source = ROOT / "README.md"
    options = _WriterCreateOptions(
        format_id.encode(), variant.encode(), filesystem.encode())
    inputs = (_WriterInputFile * 1)(
        (str(source).encode(), b"README.MD"))
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        status = writer_lib.peare_writer_add_files_to_ex(
            writer, str(output).encode(), C.byref(options), 0, inputs, 1)
        assert status == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)

    targets = _fat_writer_targets(writer_lib, output)
    expected_filesystem = {"ext2": "Ext2", "ext3": "Ext3", "ext4": "Ext4"}[filesystem]
    assert len(targets) == 1 and expected_filesystem in targets[0], targets
    capabilities = C.c_uint32(0)
    assert writer_lib.peare_writer_get_update_capabilities(
        str(output).encode(), 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f
    cud = _fat_writer_cud(writer_lib, output, tmp_path)
    opened_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, output, "EXT", max_depth=1)
    }
    readme = next(path for path in opened_rows
                  if _fat_path_matches(path, "README.MD"))
    final = next(path for path in opened_rows
                 if _fat_path_matches(path, cud["final_name"]))
    assert opened_rows[readme] == hashlib.sha256(source.read_bytes()).hexdigest()
    assert opened_rows[final] == hashlib.sha256(
        cud["replacement_payload"]).hexdigest()

    oracle_image = output
    if format_id != "raw-disk":
        oracle_image = tmp_path / f"{filesystem}-oracle-{format_id}-{variant}.img"
        if four_kn:
            _assert_windows_vhdx_oracle(output, 4096)
            converted = tmp_path / f"{filesystem}-oracle-{variant}.vmdk"
            conversion = subprocess.run(
                [vmware_tool, "-r", str(output), "-t", "0", str(converted)],
                capture_output=True, text=True, timeout=300)
            assert conversion.returncode == 0, conversion.stdout + conversion.stderr
            conversion = subprocess.run(
                [qemu_img, "convert", "-f", "vmdk", "-O", "raw",
                 str(converted), str(oracle_image)],
                capture_output=True, text=True, timeout=180)
        else:
            info = subprocess.run(
                [qemu_img, "info", "-f", qemu_format, "--output=json", str(output)],
                capture_output=True, text=True, timeout=60)
            assert info.returncode == 0, info.stdout + info.stderr
            assert json.loads(info.stdout)["format"] == qemu_format
            if format_id == "vhdx":
                _assert_windows_vhdx_oracle(output)
            conversion = subprocess.run(
                [qemu_img, "convert", "-f", qemu_format, "-O", "raw",
                 str(output), str(oracle_image)],
                capture_output=True, text=True, timeout=180)
        assert conversion.returncode == 0, conversion.stdout + conversion.stderr
    else:
        mmls = Path(sleuthkit_bin) / "mmls.exe"
        listing = subprocess.run(
            [str(mmls), "-i", "raw", str(output)],
            capture_output=True, text=True, timeout=60)
        assert listing.returncode == 0, listing.stdout + listing.stderr
        assert str(partition_offset) in listing.stdout

    fsstat = subprocess.run(
        [str(Path(sleuthkit_bin) / "fsstat.exe"), "-i", "raw", "-b",
         "4096" if four_kn else "512",
         "-o", str(partition_offset), "-f", "ext", str(oracle_image)],
        capture_output=True, text=True, timeout=60)
    assert fsstat.returncode == 0, fsstat.stdout + fsstat.stderr
    assert f"File System Type: {expected_filesystem}" in fsstat.stdout
    if filesystem in ("ext3", "ext4"):
        assert "Unmounted properly" in fsstat.stdout
    recovered = tmp_path / f"tsk-{filesystem}-{format_id}-{variant}"
    recovered.mkdir()
    extraction = subprocess.run(
        [str(Path(sleuthkit_bin) / "tsk_recover.exe"), "-i", "raw", "-b",
         "4096" if four_kn else "512",
         "-o", str(partition_offset), "-f", "ext", "-e",
         str(oracle_image), str(recovered)],
        capture_output=True, text=True, timeout=120)
    assert extraction.returncode == 0, extraction.stdout + extraction.stderr
    recovered_readme = next(recovered.rglob("README.MD"))
    recovered_final = next(recovered.rglob(cud["final_name"]))
    assert _sha256_file(recovered_readme) == hashlib.sha256(source.read_bytes()).digest()
    assert _sha256_file(recovered_final) == hashlib.sha256(
        cud["replacement_payload"]).digest()

def test_fat16_lumia_efi_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_FAT16_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT16_TEST_IMAGE to a disposable extracted FAT16 image")
    image = tmp_path / "Lumia-EFIESP-copy.img"
    shutil.copyfile(sample, image)
    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = {row["path"]: row["sha256"] for row in before.normalized_tree()}
    finally:
        before.close()
    source = tmp_path / "Lumia EFI test payload.txt"
    payload = ("FAT16 LFN validation " + uuid.uuid4().hex).encode("ascii")
    source.write_bytes(payload)
    entry_name = "System Volume Information/Peare validation " + uuid.uuid4().hex + ".txt"

    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1
    _fat_writer_add(writer_lib, image, source, entry_name)
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, entry_name)
    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
        assert rows[entry_name] == hashlib.sha256(payload).hexdigest()
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        after.close()

def test_fat16_dos_vhd_long_name_rejection_is_atomic(
        writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VHD_DYNAMIC_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VHD_DYNAMIC_TEST_IMAGE to the local DOS FAT16 VHD")
    image = tmp_path / "DOS-Win3-FAT16-copy.vhd"
    original_hash = _copy_file_sha256(sample, image)
    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = {
            row["path"]: row["sha256"] for row in before.normalized_tree()
        }
    finally:
        before.close()
    assert original_rows

    source = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    entry_name = "Peare FAT16 long name " + uuid.uuid4().hex + ".txt"
    _fat_writer_add(writer_lib, image, source, entry_name)
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, entry_name)

    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
        added = [path for path in rows if _fat_path_matches(path, entry_name)]
        assert len(added) == 1
        assert rows[added[0]] == hashlib.sha256(source.read_bytes()).hexdigest()
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        after.close()
    assert _sha256_file(sample) == original_hash

def _gui_cud_scenario(view, gui_root, payload_add, payload_keep,
                      payload_replace, target_index=0, dos_83=False):
    def item_path(path):
        return f"{gui_root}/{path}" if gui_root else path

    directory = "PEARECUD" if dos_83 else "PeareUiCud"
    added_name = Path(payload_add).name
    keep_name = Path(payload_keep).name
    renamed_name = "RENAMED.TXT" if dos_83 else "renamed.txt"
    delete_name = "DELETE.TXT" if dos_83 else "delete-me.txt"
    child_name = "SUBDIR" if dos_83 else "subdirectory"
    operations = [
        {"type": "create_directory", "name": directory,
         "expect_present": item_path(directory)},
        {"type": "add_files", "view_path": item_path(directory),
         "destination": directory,
         "sources": [str(payload_add), str(payload_keep)],
         "expect_present": item_path(f"{directory}/{added_name}")},
        {"type": "rename", "path": item_path(f"{directory}/{added_name}"),
         "view_path": item_path(directory), "name": renamed_name,
         "expect_present": item_path(f"{directory}/{renamed_name}"),
         "expect_absent": item_path(f"{directory}/{added_name}")},
        {"type": "replace", "path": item_path(f"{directory}/{renamed_name}"),
         "view_path": item_path(directory), "source": str(payload_replace),
         "expect_present": item_path(f"{directory}/{renamed_name}")},
        {"type": "create_file", "parent": item_path(directory),
         "view_path": item_path(directory), "name": delete_name,
         "expect_present": item_path(f"{directory}/{delete_name}")},
        {"type": "delete", "path": item_path(f"{directory}/{delete_name}"),
         "view_path": item_path(directory),
         "expect_absent": item_path(f"{directory}/{delete_name}")},
        {"type": "create_directory", "parent": item_path(directory),
         "view_path": item_path(directory), "name": child_name,
         "expect_present": item_path(f"{directory}/{child_name}")},
        {"type": "delete", "path": item_path(f"{directory}/{child_name}"),
         "view_path": item_path(directory),
         "expect_absent": item_path(f"{directory}/{child_name}")},
    ]
    if gui_root:
        operations[0]["parent"] = gui_root
    return {"view": view, "target_index": target_index, "operations": operations}


def _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, gui_root, target_index=0,
                      oracle_preserved_paths=(), tree_max_depth=8,
                      dos_83=False, timeout_seconds=180):
    sample_hash = _sha256_file(sample)
    image_path = str(sample).encode()
    assert writer_lib.peare_writer_can_add_files(image_path) == 1
    capabilities = C.c_uint32()
    assert writer_lib.peare_writer_get_update_capabilities(
        image_path, target_index, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3f
    before = peare_lib.open_file(sample)
    assert before is not None
    try:
        original_rows = {row["path"]: row["sha256"]
                         for row in before.normalized_tree(
                             max_depth=tree_max_depth)}
    finally:
        before.close()

    oracle_before = None
    sleuthkit_bin = os.environ.get("PEARE_SLEUTHKIT_BIN")
    if oracle_preserved_paths:
        if not sleuthkit_bin or not (Path(sleuthkit_bin) / "tsk_recover.exe").is_file():
            pytest.skip("Sleuth Kit is required for the EXT4 unwritten-extent oracle")
        oracle_before = _tsk_recover_ext(
            sleuthkit_bin, Path(sample), 0, tmp_path / "gui-writer-oracle-before")
        assert all(path in oracle_before for path in oracle_preserved_paths)
        original_hashes = {path.casefold(): digest
                           for path, digest in original_rows.items()}
        for path in oracle_preserved_paths:
            assert original_hashes[path.casefold()] == oracle_before[path].hex()

    payload_add = ROOT / "README.md"
    payload_keep = ROOT / "AGENTS.md"
    payload_replace = ROOT / "docs" / "SUPPORTED_FORMATS.md"
    if dos_83:
        payload_dir = tmp_path / "dos83-payloads"
        payload_dir.mkdir()
        payload_add = payload_dir / "ADDME.TXT"
        payload_keep = payload_dir / "KEEP.MD"
        payload_replace = payload_dir / "REPLACE.MD"
        shutil.copyfile(ROOT / "README.md", payload_add)
        shutil.copyfile(ROOT / "AGENTS.md", payload_keep)
        shutil.copyfile(ROOT / "docs" / "SUPPORTED_FORMATS.md", payload_replace)
    disposable_dir = tmp_path / "gui-writer" / view
    scenario_path = tmp_path / f"gui-writer-{view}.json"
    scenario = _gui_cud_scenario(view, gui_root, payload_add, payload_keep,
                                 payload_replace, target_index, dos_83)
    scenario_path.write_text(json.dumps(scenario, indent=2), encoding="utf-8")
    runner = ROOT / "tests" / "run_gui_writer_tests.py"
    result = subprocess.run(
        [sys.executable, str(runner), "--exe", str(peare_gui_path),
         "--image", sample, "--scenario", str(scenario_path),
         "--output-dir", str(disposable_dir), "--timeout",
         str(timeout_seconds)],
        capture_output=True, text=True, timeout=timeout_seconds + 60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert _sha256_file(sample) == sample_hash

    report = json.loads((disposable_dir / "writer-test-report.json").read_text(
        encoding="utf-8"))
    image = Path(report["image"])
    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"]
                for row in after.normalized_tree(max_depth=tree_max_depth)}
        assert {path: rows[path] for path in original_rows} == original_rows

        if oracle_before is not None:
            oracle_after = _tsk_recover_ext(
                sleuthkit_bin, image, 0, tmp_path / "gui-writer-oracle-after")
            assert {path: oracle_after.get(path) for path in oracle_before} == oracle_before
            updated_hashes = {path.casefold(): digest for path, digest in rows.items()}
            for path in oracle_preserved_paths:
                assert updated_hashes[path.casefold()] == oracle_after[path].hex()

        def path_hash(expected):
            match = next(path for path in rows
                         if path.casefold() == expected.casefold())
            return rows[match]

        prefix = f"{gui_root}/" if gui_root else ""
        directory = "PEARECUD" if dos_83 else "PeareUiCud"
        keep_name = "KEEP.MD" if dos_83 else "AGENTS.md"
        renamed_name = "RENAMED.TXT" if dos_83 else "renamed.txt"
        delete_name = "DELETE.TXT" if dos_83 else "delete-me.txt"
        child_name = "SUBDIR" if dos_83 else "subdirectory"
        add_name = "ADDME.TXT" if dos_83 else "README.md"
        assert path_hash(prefix + f"{directory}/{keep_name}") == hashlib.sha256(
            payload_keep.read_bytes()).hexdigest()
        assert path_hash(prefix + f"{directory}/{renamed_name}") == hashlib.sha256(
            payload_replace.read_bytes()).hexdigest()
        assert all(path.casefold() not in {
            (prefix + f"{directory}/{delete_name}").casefold(),
            (prefix + f"{directory}/{child_name}").casefold(),
            (prefix + f"{directory}/{add_name}").casefold(),
        } for path in rows)
    finally:
        after.close()


@pytest.mark.parametrize("view", ["tree", "details"])
def test_fat16_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                 tmp_path, view):
    sample = os.environ.get("PEARE_FAT16_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT16_TEST_IMAGE to a real FAT16 sample")
    before = peare_lib.open_file(sample)
    assert before is not None
    try:
        gui_root = next(row["path"] for row in before.normalized_tree(max_depth=1)
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        before.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, gui_root)


@pytest.mark.parametrize("view", ["tree", "details"])
def test_fat12_dos_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                     tmp_path, view):
    sample = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to the real DOS FAT12 floppy")
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, "", dos_83=True)

@pytest.mark.parametrize("view", ["tree", "details"])
def test_xdf_gui_cud_preserves_compatibility_region(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view):
    sample = None
    original = None
    for candidate in _dsk_samples_from_env():
        if not candidate or not candidate.is_file():
            continue
        data = candidate.read_bytes()
        if len(data) < 512:
            continue
        boot = data[:512]
        bps = struct.unpack_from("<H", boot, 11)[0]
        reserved = struct.unpack_from("<H", boot, 14)[0]
        fat_sectors = struct.unpack_from("<H", boot, 22)[0]
        compat_offset = (reserved + fat_sectors) * bps
        if (boot[3:11] == b"IBM 20.0" and
                data[compat_offset + 3:compat_offset + 11] == b"XDFv1.3i"):
            sample = candidate
            original = data
            break
    if sample is None:
        pytest.skip("Set PEARE_XDF_TEST_IMAGE to a local OS/2 XDF floppy")

    bps = struct.unpack_from("<H", original, 11)[0]
    reserved = struct.unpack_from("<H", original, 14)[0]
    fat_sectors = struct.unpack_from("<H", original, 22)[0]
    compat_offset = (reserved + fat_sectors) * bps
    compat_size = fat_sectors * bps
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(sample), view, "", dos_83=True)
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    updated = Path(report["image"]).read_bytes()
    assert updated[compat_offset:compat_offset + compat_size] == \
        original[compat_offset:compat_offset + compat_size]

@pytest.mark.parametrize("view", ["tree", "details"])
def test_mtcp_vmdk_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                     tmp_path, view):
    sample = os.environ.get("PEARE_VMDK_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VMDK_TEST_IMAGE to the real DOS mTCP VMDK")
    before = peare_lib.open_file(sample)
    assert before is not None
    try:
        rows = before.normalized_tree(max_depth=1)
        gui_root = next(row["path"] for row in rows
                        if row["format"] == "FAT" and row["is_container"])
    finally:
        before.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, gui_root)


@pytest.mark.parametrize("view", ["tree", "details"])
def test_ext4_depth3_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                       tmp_path, view):
    sample = os.environ.get("PEARE_EXT4_DEPTH3_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT4_DEPTH3_TEST_IMAGE to the e2fsprogs image")
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, "", oracle_preserved_paths=("DEEP.EXT",))


@pytest.mark.parametrize("view", ["tree", "details"])
def test_ext4_feature_profile_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view):
    sample = os.environ.get("PEARE_EXT4_GUI_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT4_GUI_TEST_IMAGE to the local e2fsprogs image")
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, "")


@pytest.mark.parametrize("view", ["tree", "details"])
def test_ntfs_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                tmp_path, view):
    sample = os.environ.get("PEARE_NTFS_GUI_TEST_IMAGE") or os.environ.get(
        "PEARE_VHDX_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_NTFS_GUI_TEST_IMAGE to an NTFS image")

    targets = _fat_writer_targets(writer_lib, sample)
    target_index = next((index for index, name in enumerate(targets)
                         if "NTFS" in name), None)
    assert target_index is not None, targets
    partition_match = re.search(r"partition\s+(\d+)", targets[target_index], re.I)

    before = peare_lib.open_file(sample)
    assert before is not None
    try:
        rows = before.normalized_tree(max_depth=1)
        gui_root = next(row["path"] for row in rows
                        if row["format"] == "NTFS" and row["is_container"] and
                        (not partition_match or row["path"].casefold().startswith(
                            f"partition {partition_match.group(1)}".casefold())))
    finally:
        before.close()

    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, gui_root, target_index=target_index,
                      tree_max_depth=2)


@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize("filesystem", ["Ext2", "Ext3"])
def test_chr_ext_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                   tmp_path, view, filesystem):
    sample = os.environ.get("PEARE_EXT_DISK_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXT_DISK_TEST_IMAGE to the real CHR disk")

    targets = _fat_writer_targets(writer_lib, sample)
    target_index = next((index for index, name in enumerate(targets)
                         if filesystem.casefold() in name.casefold()), None)
    assert target_index is not None, targets
    partition_match = re.search(r"partition\s+(\d+)", targets[target_index], re.I)
    assert partition_match, targets[target_index]
    partition_prefix = f"Partition {partition_match.group(1)}".casefold()

    before = peare_lib.open_file(sample)
    assert before is not None
    try:
        rows = before.normalized_tree(max_depth=1)
        gui_root = next(row["path"] for row in rows
                        if row["is_container"] and
                        row["path"].casefold().startswith(partition_prefix))
    finally:
        before.close()

    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, gui_root, target_index=target_index,
                      tree_max_depth=2)


@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize("filesystem", ["Ext2", "Ext3"])
def test_ext_volume_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                      tmp_path, view, filesystem):
    sample = os.environ.get(f"PEARE_{filesystem.upper()}_GUI_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip(f"Set PEARE_{filesystem.upper()}_GUI_TEST_IMAGE to an e2fsprogs image")
    targets = _fat_writer_targets(writer_lib, sample)
    target_index = next((index for index, name in enumerate(targets)
                         if filesystem.casefold() in name.casefold()), None)
    assert target_index is not None, targets
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, "", target_index=target_index)


@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize("filesystem", ["FAT", "Ext4"])
def test_chr_7207_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                    tmp_path, view, filesystem):
    sample = os.environ.get("PEARE_CHR_7_20_7_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_TEST_IMAGE to the extracted CHR image")

    targets = _fat_writer_targets(writer_lib, sample)
    target_index = next((index for index, name in enumerate(targets)
                         if filesystem.casefold() in name.casefold()), None)
    assert target_index is not None, targets
    partition_match = re.search(r"partition\s+(\d+)", targets[target_index], re.I)
    assert partition_match, targets[target_index]
    partition_prefix = f"Partition {partition_match.group(1)}".casefold()

    opener = peare_lib.open_file(sample)
    assert opener is not None
    try:
        gui_root = next(row["path"] for row in opener.normalized_tree(max_depth=1)
                        if row["is_container"] and
                        row["path"].casefold().startswith(partition_prefix))
    finally:
        opener.close()

    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, gui_root, target_index=target_index,
                      tree_max_depth=2)

@pytest.mark.parametrize("view", ["tree", "details"])
def test_exfat_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                 tmp_path, view):
    sample = os.environ.get("PEARE_EXFAT_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_EXFAT_TEST_IMAGE to the exFAT sample")
    targets = _fat_writer_targets(writer_lib, sample)
    target_index = next((index for index, name in enumerate(targets)
                         if "exfat" in name.casefold()), None)
    assert target_index is not None, targets
    opener = peare_lib.open_file(sample)
    assert opener is not None
    try:
        gui_root = next(row["path"] for row in opener.normalized_tree(max_depth=1)
                        if row["format"] == "EXFAT" and row["is_container"])
    finally:
        opener.close()
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, gui_root, target_index=target_index)


@pytest.mark.parametrize("view", ["tree", "details"])
def test_chr_fat32_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                     tmp_path, view):
    sample = os.environ.get("PEARE_CHR_7_20_7_LARGE_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_CHR_7_20_7_LARGE_TEST_IMAGE to the CHR disk")

    targets = _fat_writer_targets(writer_lib, sample)
    target_index = next((index for index, name in enumerate(targets)
                         if name.startswith("MBR partition 1 FAT ")), None)
    assert target_index is not None, targets
    with open(sample, "rb") as source:
        mbr = source.read(512)
        assert mbr[510:512] == b"\x55\xaa"
        partition = mbr[446:462]
        start_sector = struct.unpack_from("<I", partition, 8)[0]
        source.seek(start_sector * 512)
        boot_sector = source.read(512)
    assert boot_sector[510:512] == b"\x55\xaa"
    assert struct.unpack_from("<H", boot_sector, 17)[0] == 0
    assert struct.unpack_from("<H", boot_sector, 22)[0] == 0
    assert struct.unpack_from("<I", boot_sector, 36)[0] > 0
    partition_match = re.search(r"partition\s+(\d+)", targets[target_index], re.I)
    assert partition_match, targets[target_index]
    partition_prefix = f"Partition {partition_match.group(1)}".casefold()

    before = peare_lib.open_file(sample)
    assert before is not None
    try:
        rows = before.normalized_tree(max_depth=1)
        gui_root = next(row["path"] for row in rows
                        if row["is_container"] and
                        row["path"].casefold().startswith(partition_prefix))
    finally:
        before.close()

    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      sample, view, gui_root, target_index=target_index,
                      tree_max_depth=2)


@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize("suffix", (
    "jar", "war", "ear", "aar", "apk", "apks", "aab", "epub", "docx",
    "xlsx", "pptx", "odt", "ods", "odp", "ipa", "ipsw", "xpi", "nupkg",
    "snupkg", "whl", "vsix", "cbz", "kmz"))
def test_zip_package_alias_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, suffix, view):
    seven_zip = _seven_zip_executable()
    if not seven_zip:
        pytest.skip("7-Zip is required to create ZIP alias samples")
    archive = tmp_path / f"external-{suffix}.{suffix}"
    if suffix == "apk":
        apk_sample = Path(os.environ.get(
            "PEARE_ANDROID_APK_SAMPLE",
            Path.home() / "Documents" / "inerente a" / "Peare" /
            "validation-artifacts" / "android-apk" /
            "org.fdroid.fdroid_1023052.apk"))
        if not apk_sample.is_file():
            pytest.skip(f"Missing real Android APK sample: {apk_sample}")
        shutil.copyfile(apk_sample, archive)
    else:
        created = subprocess.run(
            [seven_zip, "a", "-tzip", "-mx=0", str(archive), "README.md"],
            cwd=ROOT, capture_output=True, text=True, timeout=60)
        assert created.returncode == 0, created.stdout + created.stderr
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(archive), view, gui_root="")

@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize("archive", [
    ROOT / "tests" / "formats" / "floppy" / "fixtures" / "FD14-Floppy.zip",
    ROOT / "tests" / "formats" / "tar" / "fixtures" / "docs.tar",
    "tar-v7",
], ids=["zip", "tar-ustar", "tar-v7"])
def test_archive_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                   tmp_path, archive, view):
    if archive == "tar-v7":
        tar_tool = os.environ.get("PEARE_GNU_TAR", "")
        if not tar_tool or not Path(tar_tool).is_file():
            pytest.skip("Set PEARE_GNU_TAR to GNU tar for V7 sample creation")
        if not Path(tar_tool).with_name("cygpath.exe").is_file():
            pytest.skip("The GNU tar runtime directory must include cygpath.exe")
        archive = tmp_path / "gnu-tar-v7-gui-sample.tar"
        _create_gnu_tar_v7_sample(
            tar_tool, archive, ROOT / "docs" / "SUPPORTED_FORMATS.md")
    assert archive.is_file()
    _run_gui_cud_case(
        writer_lib, peare_lib, peare_gui_path, tmp_path,
        str(archive), view, gui_root="")


@pytest.mark.parametrize(("sample_name", "magic"), [
    ("ascii-3.0-1.src.cpio", b"070701"), ("pcmcia", b"070702"),
    ("cinema.cpi", b"070707"),
    ("libarchive/test_read_format_cpio_afio.cpio", b"070707"),
    ("libarchive/test_read_format_cpio_bin_le.cpio", b"\xc7\x71"),
    ("libarchive/test_read_format_cpio_bin_be.cpio", b"\x71\xc7")])
@pytest.mark.parametrize("view", ["tree", "details"])
def test_cpio_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                tmp_path, sample_name, magic, view):
    root = Path(os.environ.get(
        "PEARE_CPIO_SAMPLE_ROOT",
        Path.home() / "Documents" / "inerente a" / "Peare" / "cpio-samples"))
    sample = root / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external CPIO sample: {sample}")
    assert sample.read_bytes()[:len(magic)] == magic
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(sample), view, "")


@pytest.mark.parametrize("variant", ["gnu", "bsd"])
@pytest.mark.parametrize("view", ["tree", "details"])
def test_ar_gui_flat_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                   tmp_path, variant, view):
    ar = shutil.which("ar.exe") or shutil.which("ar")
    if not ar:
        pytest.skip("GNU ar is required as an independent archive oracle")
    archive = tmp_path / f"gui-{variant}.a"
    original = tmp_path / "original.txt"
    original.write_bytes(b"original GUI archive content\n")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"ar", variant.encode(), str(archive).encode()) == 0
        assert writer_lib.peare_writer_create_file(
            writer, str(original).encode(), b"original.txt") == 0
        assert writer_lib.peare_writer_finish(writer) == 0
    finally:
        writer_lib.peare_writer_destroy(writer)

    sample_hash = _sha256_file(archive)
    added = tmp_path / "added.txt"
    replacement = tmp_path / "replacement.txt"
    added.write_bytes(b"added from GUI\n")
    replacement.write_bytes(b"replaced from GUI\n")
    scenario = {
        "view": view,
        "target_index": 0,
        "operations": [
            {"type": "add_files", "view_path": "", "destination": "",
             "sources": [str(added)], "expect_present": "added.txt"},
            {"type": "rename", "path": "original.txt", "view_path": "",
             "name": "renamed.txt", "expect_present": "renamed.txt",
             "expect_absent": "original.txt"},
            {"type": "replace", "path": "renamed.txt", "view_path": "",
             "source": str(replacement), "expect_present": "renamed.txt"},
            {"type": "create_file", "parent": "", "view_path": "",
             "name": "delete-me.txt", "expect_present": "delete-me.txt"},
            {"type": "delete", "path": "delete-me.txt", "view_path": "",
             "expect_absent": "delete-me.txt"},
        ],
    }
    scenario_path = tmp_path / f"ar-gui-{view}.json"
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
    output_dir = tmp_path / f"ar-gui-output-{view}"
    result = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
         "--exe", str(peare_gui_path), "--image", str(archive),
         "--scenario", str(scenario_path), "--output-dir", str(output_dir)],
        capture_output=True, text=True, timeout=240)
    assert result.returncode == 0, result.stdout + result.stderr
    assert _sha256_file(archive) == sample_hash
    report = json.loads((output_dir / "writer-test-report.json").read_text(
        encoding="utf-8"))
    updated = Path(report["image"])
    names = subprocess.run([ar, "t", str(updated)], check=True,
                           capture_output=True).stdout.decode("utf-8").splitlines()
    assert names == ["renamed.txt", "added.txt"]

@pytest.mark.parametrize("view", ["tree", "details"])
@pytest.mark.parametrize(("compression", "expected_method"), [
    ("store", zipfile.ZIP_STORED),
    ("deflate", zipfile.ZIP_DEFLATED),
    ("bzip2", zipfile.ZIP_BZIP2),
    ("lzma", zipfile.ZIP_LZMA),
    ("xz", 95),
    ("zstd", 93),
    ("ppmd", 98),
    ("shrink", 1),
    ("deflate64", 9),
])
def test_zip_compression_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path,
        compression, expected_method, view):
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        seven_zip = str(candidate) if candidate.is_file() else None
    if not seven_zip and compression not in {"zstd", "ppmd", "deflate64"}:
        pytest.skip("7-Zip is required to create independent ZIP GUI samples")

    archive = tmp_path / f"gui-{compression}.zip"
    level = "0" if compression == "store" else "6"
    method = [f"-mm={compression}"] \
        if compression in {"bzip2", "lzma", "xz"} else []
    if compression in {"xz", "zstd", "ppmd", "shrink", "deflate64"}:
        options = _WriterCreateOptions(b"zip", compression.encode(), None)
        inputs = (_WriterInputFile * 2)(
            _WriterInputFile(str(ROOT / "README.md").encode(), b"README.md"),
            _WriterInputFile(str(ROOT / "AGENTS.md").encode(), b"AGENTS.md"))
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            created = writer_lib.peare_writer_add_files_to_ex(
                writer, str(archive).encode(), C.byref(options), 0,
                inputs, len(inputs))
            assert created == 0, writer_lib.peare_writer_last_error(writer)
        finally:
            writer_lib.peare_writer_destroy(writer)
    else:
        created = subprocess.run(
            [seven_zip, "a", "-tzip", f"-mx={level}", *method, str(archive),
             "README.md", "AGENTS.md"], cwd=ROOT, capture_output=True,
            text=True, timeout=60)
        assert created.returncode == 0, created.stdout + created.stderr
    with zipfile.ZipFile(archive) as source:
        if compression not in {"xz", "zstd", "ppmd", "shrink", "deflate64"}:
            assert source.testzip() is None
        assert {item.compress_type for item in source.infolist()} == {
            expected_method}

    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(archive), view, "")
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    with zipfile.ZipFile(report["image"]) as updated:
        assert updated.getinfo("AGENTS.md").compress_type == expected_method
        if compression not in {"xz", "zstd", "ppmd", "shrink", "deflate64"}:
            assert updated.testzip() is None
            assert updated.read("AGENTS.md") == (ROOT / "AGENTS.md").read_bytes()
    if compression in {"xz", "zstd", "ppmd"}:
        winrar = _winrar_executable()
        tools = [_seven_zip_executable()]
        if winrar:
            tools.append(winrar)
        assert all(tools)
        independently_validated = False
        for tool in tools:
            arguments = [tool, "t", "-ibck", report["image"]] \
                if tool == winrar else [tool, "t", report["image"]]
            checked = subprocess.run(arguments, capture_output=True,
                                     text=True, timeout=120, check=False)
            if checked.returncode == 0:
                independently_validated = True
                break
            if "Unsupported Method" not in (checked.stdout + checked.stderr):
                assert checked.returncode == 0, checked.stdout + checked.stderr
        if not independently_validated:
            pytest.skip(f"No installed archive tool supports ZIPX {compression}")
    elif compression == "shrink":
        seven_zip_check = subprocess.run(
            [seven_zip, "t", report["image"]], capture_output=True,
            text=True, timeout=120, check=False)
        assert seven_zip_check.returncode == 0, \
            seven_zip_check.stdout + seven_zip_check.stderr
        with peare_lib.open_file(report["image"]) as opener:
            rows = {row["path"]: row for row in opener.normalized_tree()}
            assert rows["AGENTS.md"]["sha256"] == hashlib.sha256(
                (ROOT / "AGENTS.md").read_bytes()).hexdigest()
    elif compression == "deflate64":
        result = subprocess.run([seven_zip, "t", report["image"]],
                                capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stdout + result.stderr
        with peare_lib.open_file(report["image"]) as opener:
            rows = {row["path"]: row for row in opener.normalized_tree()}
            assert rows["AGENTS.md"]["sha256"] == hashlib.sha256(
                (ROOT / "AGENTS.md").read_bytes()).hexdigest()

@pytest.mark.parametrize("view", ["tree", "details"])
def test_zip_shrink_gui_cud_offscreen(
        writer_lib, peare_lib, peare_gui_path, tmp_path, view):
    samples = [sample for sample in _zip_shrink_samples_from_env()
               if sample is not None and sample.is_file()]
    if not samples:
        pytest.skip("Set PEARE_ZIP_SHRINK_TEST_IMAGES to local Shrink ZIP samples")
    sample = samples[0]
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(sample), view, "")
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        seven_zip = str(candidate) if candidate.is_file() else None
    if not seven_zip:
        pytest.skip("7-Zip is required to validate the updated Shrink archive")
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    validation = subprocess.run(
        [seven_zip, "t", report["image"]], capture_output=True, text=True,
        timeout=120, check=False)
    assert validation.returncode == 0, validation.stdout + validation.stderr

@pytest.mark.parametrize("view", ["tree", "details"])
def test_zip64_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                                 tmp_path, view):
    sample = _zip64_sample_from_env()
    if sample is None:
        sample = ROOT / "tmp" / "writer-independent-samples" / "zip64-count.zip"
    if not sample.is_file():
        pytest.skip("Set PEARE_ZIP64_TEST_IMAGE to the independent ZIP64 sample")
    _run_gui_cud_case(writer_lib, peare_lib, peare_gui_path, tmp_path,
                      str(sample), view, "", timeout_seconds=300)
    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 shutil.which("7zz"))
    if not seven_zip and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "7-Zip" / "7z.exe"
        seven_zip = str(candidate) if candidate.is_file() else None
    if not seven_zip:
        pytest.skip("7-Zip is required to validate the updated ZIP64 archive")
    report = json.loads((tmp_path / "gui-writer" / view /
                         "writer-test-report.json").read_text(encoding="utf-8"))
    validation = subprocess.run(
        [seven_zip, "t", report["image"]], capture_output=True, text=True,
        timeout=180, check=False)
    assert validation.returncode == 0, validation.stdout + validation.stderr


def test_vhdx_lumia_exposes_explicit_fat_targets(writer_lib):
    sample = os.environ.get("PEARE_VHDX_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VHDX_TEST_IMAGE to the Lumia VHDX sample")
    image = Path(sample)
    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1
    targets = _fat_writer_targets(writer_lib, image)
    assert len(targets) >= 2
    assert any(name.startswith("GPT partition 34 FAT") for name in targets)

def test_vhdx_lumia_writes_selected_volume_on_copy(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VHDX_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VHDX_TEST_IMAGE to the Lumia VHDX sample")
    source_hash = _sha256_file(sample)
    image = tmp_path / "Lumia-VHDX-copy.vhdx"
    shutil.copyfile(sample, image)
    targets = _fat_writer_targets(writer_lib, image)
    target_indices = [i for i, name in enumerate(targets)
                      if name.startswith("GPT partition") and " FAT " in name]
    assert len(target_indices) == 5, targets

    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = before.normalized_tree(max_depth=1)
    finally:
        before.close()

    expected = []
    for target_index in target_indices:
        source = tmp_path / f"Lumia VHDX validation {target_index}.txt"
        payload = (f"Lumia VHDX writer {target_index} " + uuid.uuid4().hex).encode("ascii")
        source.write_bytes(payload)
        entry_name = f"Peare validation {target_index} {uuid.uuid4().hex}.txt"
        _fat_writer_add_target(writer_lib, image, source, entry_name, target_index)
        cud = _fat_writer_cud(writer_lib, image, tmp_path, target_index)
        expected.append((entry_name, payload, cud))

    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row for row in after.normalized_tree(max_depth=1)}
        for entry_name, payload, cud in expected:
            added_path = next(path for path in rows
                              if path.endswith("/" + entry_name))
            assert rows[added_path]["sha256"] == hashlib.sha256(payload).hexdigest()
            cud_path = next(path for path in rows
                            if _fat_path_matches(path, cud["final_name"]))
            assert rows[cud_path]["sha256"] == hashlib.sha256(
                cud["replacement_payload"]).hexdigest()
            assert not any(_fat_path_matches(path, name)
                           for path in rows for name in (
                               cud["created_name"], cud["empty_name"],
                               cud["directory_name"],
                               cud["renamed_directory_name"]))
        original_by_path = {row["path"]: row for row in original_rows}
        assert all(rows[path] == row for path, row in original_by_path.items())
    finally:
        after.close()
    assert _sha256_file(sample) == source_hash

def test_fat_dos_83_name_on_real_dos_image(writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to a disposable DOS FAT image")
    image = tmp_path / "DOS-6.22-copy.img"
    shutil.copyfile(sample, image)
    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = {row["path"]: row["sha256"] for row in before.normalized_tree()}
    finally:
        before.close()
    source = tmp_path / "payload.txt"
    payload = b"DOS short-name writer check\r\n"
    source.write_bytes(payload)

    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1
    _fat_writer_add(writer_lib, image, source, "PEARE.TXT")
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, "PEARE.TXT")
    cud = _fat_writer_cud(writer_lib, image, tmp_path)

    entries = []
    image_bytes = image.read_bytes()
    for offset in range(0, len(image_bytes) - 31, 32):
        record = image_bytes[offset:offset + 32]
        if record[:11] == b"PEARE   TXT" and record[11] == 0x20:
            entries.append((offset, record))
    assert len(entries) == 1
    offset, _ = entries[0]
    assert offset < 32 or image_bytes[offset - 21] != 0x0f

    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
        assert rows["PEARE.TXT"] == hashlib.sha256(payload).hexdigest()
        cud_path = next(path for path in rows
                        if _fat_path_matches(path, cud["final_name"]))
        assert rows[cud_path] == hashlib.sha256(cud["replacement_payload"]).hexdigest()
        assert not any(_fat_path_matches(path, name)
                       for path in rows for name in (
                           cud["created_name"], cud["empty_name"],
                           cud["directory_name"],
                           cud["renamed_directory_name"]))
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        after.close()

def test_fat_writer_cud_on_local_floppy_variants(
        writer_lib, peare_lib, tmp_path):
    configured = os.environ.get("PEARE_FAT_FLOPPY_CUD_IMAGES", "")
    samples = [Path(value) for value in configured.split(os.pathsep) if value]
    samples = [sample for sample in samples if sample.is_file()]
    if not samples:
        pytest.skip("Set PEARE_FAT_FLOPPY_CUD_IMAGES to local floppy samples")

    for index, sample in enumerate(samples):
        image = tmp_path / f"floppy-{index}-{sample.name}"
        original_hash = hashlib.sha256(sample.read_bytes()).digest()
        shutil.copyfile(sample, image)
        before = peare_lib.open_file(str(image))
        assert before is not None, sample
        try:
            original_rows = {
                row["path"]: row["sha256"] for row in before.normalized_tree()
            }
        finally:
            before.close()

        cud = _fat_writer_cud(writer_lib, image, tmp_path)
        _assert_duplicate_create_is_atomic(
            writer_lib, image, [image], tmp_path, 0, cud["final_name"])
        after = peare_lib.open_file(str(image))
        assert after is not None, sample
        try:
            rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
            final_path = next(
                path for path in rows if _fat_path_matches(path, cud["final_name"])
            )
            assert rows[final_path] == hashlib.sha256(
                cud["replacement_payload"]).hexdigest(), sample
            assert not any(
                _fat_path_matches(path, name)
                for path in rows
                for name in (
                    cud["created_name"], cud["empty_name"],
                    cud["directory_name"], cud["renamed_directory_name"]
                )
            ), sample
            assert {path: rows[path] for path in original_rows} == original_rows, sample
        finally:
            after.close()
        assert hashlib.sha256(sample.read_bytes()).digest() == original_hash, sample

@pytest.mark.parametrize("sample", _dsk_samples_from_env())
def test_fat_writer_updates_os2_dsk_sample(writer_lib, peare_lib, tmp_path, sample):
    if not sample or not sample.is_file():
        pytest.skip("Set PEARE_DSK_TEST_IMAGES to local OS/2 DSK image paths")

    image = tmp_path / f"xdf-copy-{sample.name}"
    original_bytes = sample.read_bytes()
    original_hash = hashlib.sha256(original_bytes).digest()
    shutil.copyfile(sample, image)
    boot = original_bytes[:512]
    bps = struct.unpack_from("<H", boot, 11)[0]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fat_sectors = struct.unpack_from("<H", boot, 22)[0]
    compat_offset = (reserved + fat_sectors) * bps
    compat_size = fat_sectors * bps
    compatibility_image = original_bytes[compat_offset:compat_offset + compat_size]
    xdf = (boot[3:11] == b"IBM 20.0" and
           struct.unpack_from("<H", boot, 24)[0] == 23 and
           struct.unpack_from("<H", boot, 26)[0] == 2 and
           original_bytes[compat_offset + 3:compat_offset + 11] == b"XDFv1.3i")

    before = peare_lib.open_file(str(image))
    assert before is not None
    try:
        original_rows = {
            row["path"]: row["sha256"] for row in before.normalized_tree()
        }
    finally:
        before.close()

    assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1
    label = C.create_string_buffer(128)
    assert writer_lib.peare_writer_get_add_target_name(
        str(image).encode(), 0, label, len(label)) == 0
    assert (b"XDF" in label.value) == xdf
    cud = _fat_writer_cud(writer_lib, image, tmp_path)
    _assert_duplicate_create_is_atomic(
        writer_lib, image, [image], tmp_path, 0, cud["final_name"])
    after = peare_lib.open_file(str(image))
    assert after is not None
    try:
        rows = {row["path"]: row["sha256"] for row in after.normalized_tree()}
        final_path = next(
            path for path in rows if _fat_path_matches(path, cud["final_name"])
        )
        assert rows[final_path] == hashlib.sha256(
            cud["replacement_payload"]).hexdigest()
        assert {path: rows[path] for path in original_rows} == original_rows
    finally:
        after.close()
    result = image.read_bytes()
    if xdf:
        assert result[compat_offset:compat_offset + compat_size] == compatibility_image
    else:
        assert result[reserved * bps:(reserved + fat_sectors) * bps] == result[
            (reserved + fat_sectors) * bps:(reserved + 2 * fat_sectors) * bps]
    assert hashlib.sha256(sample.read_bytes()).digest() == original_hash

def test_dsk_rename_latency_and_aaru_readback(writer_lib, peare_lib, tmp_path):
    sample_value = os.environ.get("PEARE_DSK_LATENCY_TEST_IMAGE", "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_DSK_LATENCY_TEST_IMAGE to local DISK10.DSK")
    aaru = os.environ.get(
        "PEARE_AARU",
        Path.home() / "Documents" / "inerente a" / "Peare" / "Research tools" / "Aaru-5.4.2" / "app" / "aaru.exe")
    if not Path(aaru).is_file():
        pytest.skip("Aaru 5.4.2 is required for independent XDF read-back")

    sample = Path(sample_value)
    source_hash = _sha256_file(sample)
    image = tmp_path / sample.name
    shutil.copyfile(sample, image)
    target_index = _fat_writer_target_index(writer_lib, image, "FAT")
    opened = peare_lib.open_file(str(image))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
    finally:
        opened.close()
    source_path = next(path for path in rows
                       if path.upper().split("/")[-1] == "PEN.DAT")
    source_name = source_path.rsplit("/", 1)[-1]
    expected_hash = rows[source_path]
    measurements = []

    for run in range(3):
        target_name = f"PEARE{run}.DAT"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), target_index)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_rename_entry(
                writer, source_name.encode(), target_name.encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, rename_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, rename_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)
        source_name = target_name

    final = peare_lib.open_file(str(image))
    assert final is not None
    try:
        final_rows = {row["path"]: row["sha256"]
                      for row in final.normalized_tree()}
        final_path = next(path for path in final_rows
                          if path.upper().split("/")[-1] == "PEARE2.DAT")
        assert final_rows[final_path] == expected_hash
    finally:
        final.close()

    output_dir = tmp_path / "aaru-extract"
    extracted = subprocess.run(
        [aaru, "fs", "extract", str(image), str(output_dir)],
        capture_output=True, text=True, timeout=120)
    assert extracted.returncode == 0, extracted.stdout + extracted.stderr
    independent_file = next(output_dir.rglob("PEARE2.DAT"))
    assert _sha256_file(independent_file).hex() == expected_hash
    print("XDF DSK rename seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 2.0
    assert _sha256_file(sample) == source_hash

def test_fat_dos_cud_operation_latency(writer_lib, tmp_path):
    sample = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to a real DOS FAT image")
    if os.name != "nt":
        pytest.skip("Process I/O counters are available only on Windows")

    class IoCounters(C.Structure):
        _fields_ = [(name, C.c_ulonglong) for name in (
            "ReadOperationCount", "WriteOperationCount",
            "OtherOperationCount", "ReadTransferCount",
            "WriteTransferCount", "OtherTransferCount")]

    kernel32 = C.WinDLL("kernel32", use_last_error=True)
    get_io = kernel32.GetProcessIoCounters
    get_io.argtypes = (C.c_void_p, C.POINTER(IoCounters))
    get_io.restype = C.c_int

    def process_io():
        counters = IoCounters()
        assert get_io(C.c_void_p(-1), C.byref(counters))
        return counters.ReadTransferCount, counters.WriteTransferCount

    measurements = []
    sample_hash = _sha256_file(sample)
    for run in range(3):
        image = tmp_path / f"DOS-latency-{run}.img"
        shutil.copyfile(sample, image)
        payload = tmp_path / f"DOS-latency-{run}.txt"
        payload.write_bytes(b"Measured CUD payload")
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = process_io()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), 0)
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(process_io(), io_before))

            io_before = process_io()
            started = time.perf_counter()
            result = writer_lib.peare_writer_create_file(
                writer, str(payload).encode(), f"LAT{run}.TXT".encode())
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            create_seconds = time.perf_counter() - started
            create_io = tuple(after - before for after, before in
                              zip(process_io(), io_before))

            io_before = process_io()
            started = time.perf_counter()
            result = writer_lib.peare_writer_rename_entry(
                writer, f"LAT{run}.TXT".encode(),
                f"REN{run}.TXT".encode())
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(process_io(), io_before))

            io_before = process_io()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_finish(writer)
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(process_io(), io_before))
            measurements.append({
                "seconds": (begin_seconds, create_seconds, rename_seconds,
                            finish_seconds),
                "io_bytes_read_written": (begin_io, create_io, rename_io,
                                          finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

    print("DOS FAT12 CUD seconds (begin, create, rename, finish):", measurements)
    assert max(row["seconds"][2] for row in measurements) < 2.0
    assert _sha256_file(sample) == sample_hash

def test_fat_vhd_large_add_latency_and_opener_readback(
        writer_lib, peare_lib, tmp_path):
    sample = os.environ.get("PEARE_VHD_DYNAMIC_TEST_IMAGE", "")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_VHD_DYNAMIC_TEST_IMAGE to the local DOS FAT16 VHD")
    if os.name != "nt":
        pytest.skip("Process I/O counters are available only on Windows")

    payload = tmp_path / "large-write-payload.bin"
    payload.write_bytes(bytes(range(256)) * (64 * 1024))
    payload_hash = _sha256_file(payload).hex()
    sample_hash = _sha256_file(sample)
    measurements = []
    for run in range(3):
        image = tmp_path / f"fat16-large-add-{run}.vhd"
        shutil.copyfile(sample, image)
        target_index = _fat_writer_target_index(writer_lib, image, "FAT16")
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), target_index)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_create_file(
                writer, str(payload).encode(), f"LG{run:03d}.BIN".encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add = time.perf_counter() - started
            add_io = tuple(after - before for after, before in
                           zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin, add, finish),
                "io_bytes_read_written": (begin_io, add_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        opened = peare_lib.open_file(str(image))
        assert opened is not None
        try:
            rows = opened.normalized_tree()
            result = next(row for row in rows
                          if _fat_path_matches(row["path"], f"LG{run:03d}.BIN"))
            assert result["sha256"] == payload_hash
        finally:
            opened.close()

    print("FAT16 VHD 16 MiB add seconds (begin, add, finish):", measurements)
    assert max(row["seconds"][1] for row in measurements) < 5.0
    assert _sha256_file(sample) == sample_hash

@pytest.mark.parametrize(("sample_name", "filesystem"), [
    ("ext2-gdt-csum.img", "Ext2"),
    ("ext3-gdt-csum.img", "Ext3"),
    ("ext4-seed-journal-htree.img", "Ext4"),
])
def test_ext_large_add_latency_and_independent_readback(
        writer_lib, peare_lib, tmp_path, sample_name, filesystem):
    sample_dir = Path(os.environ.get("PEARE_EXT_CHECKSUM_SAMPLE_DIR", ""))
    e2fsck = os.environ.get("PEARE_E2FSCK", "")
    debugfs = os.environ.get("PEARE_DEBUGFS", "")
    sample = sample_dir / sample_name
    if not sample.is_file():
        pytest.skip("Set PEARE_EXT_CHECKSUM_SAMPLE_DIR to the e2fsprogs samples")
    if not e2fsck or not Path(e2fsck).is_file():
        pytest.skip("Set PEARE_E2FSCK to the independent e2fsck executable")
    if not debugfs or not Path(debugfs).is_file():
        pytest.skip("Set PEARE_DEBUGFS to the independent debugfs executable")

    image = tmp_path / sample.name
    source_hash = _copy_file_sha256(sample, image)
    target_index = _fat_writer_target_index(writer_lib, image, filesystem)
    payload = tmp_path / "ext-large-payload.bin"
    payload_bytes = os.urandom(16 * 1024 * 1024)
    payload.write_bytes(payload_bytes)
    payload_hash = hashlib.sha256(payload_bytes).hexdigest()
    measurements = []

    for run in range(3):
        entry_name = f"PEARE-LARGE-{filesystem.upper()}-{run}.BIN"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_files_begin_target(
                writer, str(image).encode(), target_index)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_file(
                writer, str(payload).encode(), entry_name.encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add = time.perf_counter() - started
            add_io = tuple(after - before for after, before in
                           zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin, add, finish),
                "io_bytes_read_written": (begin_io, add_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

    opened = peare_lib.open_file(str(image))
    assert opened is not None
    try:
        rows = {row["path"]: row["sha256"] for row in opened.normalized_tree()}
        for run in range(3):
            entry_name = f"PEARE-LARGE-{filesystem.upper()}-{run}.BIN"
            entry_path = next(path for path in rows
                              if _fat_path_matches(path, entry_name))
            assert rows[entry_path] == payload_hash
    finally:
        opened.close()

    tool_env = os.environ.copy()
    cygwin_root = Path(debugfs).parents[2]
    tool_env["PATH"] = os.pathsep.join((
        str(cygwin_root / "bin"), str(cygwin_root / "usr" / "bin"),
        tool_env.get("PATH", "")))
    check = subprocess.run(
        [e2fsck, "-fn", str(image)], capture_output=True, text=True,
        timeout=120, env=tool_env)
    assert check.returncode == 0, check.stdout + check.stderr
    for run in range(3):
        entry_name = f"PEARE-LARGE-{filesystem.upper()}-{run}.BIN"
        extracted = tmp_path / f"oracle-{run}.bin"
        result = subprocess.run(
            [debugfs, "-R", f"dump /{entry_name} {extracted}", str(image)],
            capture_output=True, text=True, timeout=120, env=tool_env)
        assert result.returncode == 0, result.stdout + result.stderr
        assert _sha256_file(extracted).hex() == payload_hash

    print(f"{filesystem} 16 MiB add seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 5.0
    assert max(row["seconds"][2] for row in measurements) < 10.0
    assert _sha256_file(sample) == source_hash


def _wad_lumps(path):
    data = Path(path).read_bytes()
    assert data[:4] in (b"IWAD", b"PWAD")
    count, directory = struct.unpack_from("<II", data, 4)
    assert directory >= 12 and directory + count * 16 <= len(data)
    lumps = []
    for index in range(count):
        offset, size = struct.unpack_from("<II", data, directory + index * 16)
        raw_name = data[directory + index * 16 + 8:directory + index * 16 + 16]
        assert offset + size <= len(data)
        lumps.append((raw_name.split(b"\0", 1)[0].decode("ascii"),
                      data[offset:offset + size]))
    return data[:4], lumps


def _create_test_wad(writer_lib, path, variant, members):
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_create_begin_variant(
            writer, b"wad", variant, str(path).encode()) == 0
        for index, (name, data) in enumerate(members):
            payload = Path(path).parent / f"wad-input-{index}.bin"
            payload.write_bytes(data)
            assert writer_lib.peare_writer_create_file(
                writer, str(payload).encode(), name) == 0, \
                writer_lib.peare_writer_last_error(writer)
        assert writer_lib.peare_writer_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)


@pytest.mark.parametrize(("variant", "signature"), [
    (b"iwad", b"IWAD"), (b"pwad", b"PWAD")])
def test_wad_create_roundtrip(writer_lib, peare_lib, tmp_path, variant, signature):
    archive = tmp_path / f"created-{variant.decode()}.wad"
    _create_test_wad(writer_lib, archive, variant,
                     [(b"DUPLUMP", b"first"), (b"DUPLUMP", b"second"),
                      (b"MAP01", b"")])
    actual_signature, lumps = _wad_lumps(archive)
    assert actual_signature == signature
    assert lumps == [("DUPLUMP", b"first"), ("DUPLUMP", b"second"), ("MAP01", b"")]
    with peare_lib.open_file(str(archive)) as opener:
        assert opener.container_format() == "WAD"
        assert sum(opener.resource_count(folder) for folder in range(opener.folder_count())) == 3
    batch_archive = tmp_path / f"batch-{variant.decode()}.wad"
    payload = tmp_path / f"batch-payload-{variant.decode()}"
    payload.write_bytes(b"batch payload")
    input_file = _WriterInputFile(str(payload).encode(), b"BATCH")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_add_files_to(
            writer, str(batch_archive).encode(), b"wad", variant, 0,
            C.byref(input_file), 1) == 0, writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    assert _wad_lumps(batch_archive) == (signature, [("BATCH", b"batch payload")])


def test_wad_cud_by_entry_id_on_real_and_duplicate_lumps(
        writer_lib, peare_lib, tmp_path):
    sample = ROOT / "tests" / "formats" / "wad" / "fixtures" / "e1m4.wad"
    assert sample.is_file()
    original_hash = _sha256_file(sample)
    real_copy = tmp_path / "freedoom-level-copy.wad"
    shutil.copy2(sample, real_copy)
    before_signature, before_lumps = _wad_lumps(real_copy)
    payload = tmp_path / "replacement-lump.bin"
    payload.write_bytes(b"replacement content\n")
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, str(real_copy).encode()) == 0
        assert writer_lib.peare_writer_rename_entry(
            writer, before_lumps[0][0].encode(), b"RENAMED") == 0
        assert writer_lib.peare_writer_replace_entry_by_id(
            writer, str(payload).encode(), before_lumps[1][0].encode(), 1) == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    after_signature, after_lumps = _wad_lumps(real_copy)
    assert after_signature == before_signature
    assert after_lumps[0] == ("RENAMED", before_lumps[0][1])
    assert after_lumps[1] == (before_lumps[1][0], payload.read_bytes())
    assert after_lumps[2:] == before_lumps[2:]
    assert _sha256_file(sample) == original_hash
    with peare_lib.open_file(str(real_copy)) as opener:
        assert opener.container_format() == "WAD"

    duplicate_copy = tmp_path / "duplicate-lumps.wad"
    _create_test_wad(writer_lib, duplicate_copy, b"pwad", [
        (b"DUPLUMP", b"first"), (b"DUPLUMP", b"second"),
        (b"REMOVE", b"third")])
    import peare_opener as po
    with peare_lib.open_file(str(duplicate_copy)) as opener:
        context_ids = []
        for folder in range(opener.folder_count()):
            for resource_index in range(opener.resource_count(folder)):
                resource = C.c_void_p()
                assert opener._L.peare_opener_open_resource_at(
                    opener._h, folder, resource_index, C.byref(resource)) == 0
                try:
                    context = po._Context()
                    assert opener._L.peare_resource_get_context(
                        resource, C.byref(context)) == 0
                    context_ids.append(context.base_id)
                    opener._L.peare_resource_context_free(C.byref(context))
                finally:
                    opener._L.peare_resource_destroy(resource)
        assert context_ids == [0, 1, 2]
    writer = _HANDLE()
    assert writer_lib.peare_writer_create(C.byref(writer)) == 0
    try:
        assert writer_lib.peare_writer_update_begin(writer, str(duplicate_copy).encode()) == 0
        capabilities = C.c_uint32()
        assert writer_lib.peare_writer_get_update_capabilities(
            str(duplicate_copy).encode(), 0, C.byref(capabilities)) == 0
        assert capabilities.value & 1
        assert capabilities.value & 2
        assert capabilities.value & 4 == 0
        assert capabilities.value & 8
        assert capabilities.value & 16
        assert capabilities.value & 32
        assert writer_lib.peare_writer_rename_entry(
            writer, b"DUPLUMP", b"AMBIG") == 1
        assert writer_lib.peare_writer_create_directory(writer, b"folder") == 10
        assert writer_lib.peare_writer_rename_entry_by_id(
            writer, b"DUPLUMP", b"SECOND", 1) == 0
        assert writer_lib.peare_writer_replace_entry_by_id(
            writer, str(payload).encode(), b"DUPLUMP", 0) == 0
        assert writer_lib.peare_writer_delete_entry_by_id(
            writer, b"REMOVE", 0, 2) == 0
        added = tmp_path / "added-lump.bin"
        added.write_bytes(b"added")
        assert writer_lib.peare_writer_add_file(
            writer, str(added).encode(), b"ADDED") == 0
        assert writer_lib.peare_writer_create_empty_file(writer, b"EMPTY") == 0
        assert writer_lib.peare_writer_update_finish(writer) == 0, \
            writer_lib.peare_writer_last_error(writer)
    finally:
        writer_lib.peare_writer_destroy(writer)
    signature, lumps = _wad_lumps(duplicate_copy)
    assert signature == b"PWAD"
    assert lumps == [("DUPLUMP", payload.read_bytes()), ("SECOND", b"second"),
                     ("ADDED", b"added"), ("EMPTY", b"")]
    with peare_lib.open_file(str(duplicate_copy)) as opener:
        assert opener.container_format() == "WAD"
        assert sum(opener.resource_count(folder) for folder in range(opener.folder_count())) == 4


@pytest.mark.parametrize("view", ["tree", "details"])
def test_wad_gui_cud_offscreen(writer_lib, peare_lib, peare_gui_path,
                               tmp_path, view):
    archive = ROOT / "tests" / "formats" / "wad" / "fixtures" / "e1m4.wad"
    original_hash = _sha256_file(archive)
    signature, original_lumps = _wad_lumps(archive)
    assert len(original_lumps) >= 2
    existing_names = {name for name, _ in original_lumps}
    def unique_name(prefix):
        for suffix in range(1000):
            candidate = f"{prefix}{suffix:07X}"
            if candidate not in existing_names:
                existing_names.add(candidate)
                return candidate.encode("ascii")
        raise AssertionError("Could not select an unused WAD lump name")

    added_name = b"LICENSE"
    assert added_name.decode("ascii") not in existing_names
    existing_names.add(added_name.decode("ascii"))
    renamed_name = unique_name("R")
    empty_name = unique_name("E")
    payload = ROOT / "LICENSE"
    replaced_index = 0
    scenario = {
        "view": view,
        "operations": [
            {"type": "add_files", "view_path": "", "destination": "",
             "sources": [str(payload)],
             "expect_present": added_name.decode("ascii")},
            {"type": "rename", "path": original_lumps[replaced_index][0],
             "view_path": "", "name": renamed_name.decode("ascii"),
             "expect_present": renamed_name.decode("ascii")},
            {"type": "replace", "path": renamed_name.decode("ascii"),
             "view_path": "", "source": str(payload),
             "expect_present": renamed_name.decode("ascii")},
            {"type": "create_file", "parent": "", "view_path": "",
             "name": empty_name.decode("ascii"),
             "expect_present": empty_name.decode("ascii")},
            {"type": "delete", "path": empty_name.decode("ascii"),
             "view_path": "", "expect_absent": empty_name.decode("ascii")},
        ],
    }
    scenario_path = tmp_path / f"wad-gui-{view}.json"
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
    output_dir = tmp_path / f"wad-gui-output-{view}"
    result = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
         "--exe", str(peare_gui_path), "--image", str(archive),
         "--scenario", str(scenario_path), "--output-dir", str(output_dir)],
        capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr
    assert _sha256_file(archive) == original_hash
    _, lumps = _wad_lumps(output_dir / "writer-test-copy.wad")
    expected = list(original_lumps)
    expected[replaced_index] = (renamed_name.decode("ascii"), payload.read_bytes())
    expected.append((added_name.decode("ascii"), payload.read_bytes()))
    assert signature == b"PWAD"
    assert lumps == expected

@pytest.mark.parametrize(("image_format", "subformat", "variant", "extension"), [
    ("raw", "", "raw", "img"),
    ("vpc", "subformat=fixed", "vhd-fixed", "vhd"),
    ("vpc", "subformat=dynamic", "vhd-dynamic", "vhd"),
    ("vhdx", "subformat=fixed", "vhdx-fixed", "vhdx"),
    ("vhdx", "subformat=dynamic", "vhdx-dynamic", "vhdx"),
    ("vmdk", "subformat=monolithicSparse,zeroed_grain=on",
     "vmdk-monolithic-sparse", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentSparse,zeroed_grain=on",
     "vmdk-split-sparse", "vmdk"),
    ("vmdk", "subformat=monolithicFlat", "vmdk-monolithic-flat", "vmdk"),
    ("vmdk", "subformat=twoGbMaxExtentFlat", "vmdk-split-flat", "vmdk"),
], ids=["raw", "vhd-fixed", "vhd-dynamic", "vhdx-fixed", "vhdx-dynamic",
       "vmdk-monolithic-sparse", "vmdk-split-sparse",
       "vmdk-monolithic-flat", "vmdk-split-flat"])
def test_fat_container_rename_latency_and_qemu_readback(
        writer_lib, peare_lib, tmp_path, image_format, subformat, variant,
        extension):
    sample_value = os.environ.get("PEARE_FAT_DOS_TEST_IMAGE", "")
    qemu_img = os.environ.get("PEARE_QEMU_IMG", "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_FAT_DOS_TEST_IMAGE to the real DOS FAT12 floppy")
    if image_format != "raw" and (not qemu_img or not Path(qemu_img).is_file()):
        pytest.skip("Set PEARE_QEMU_IMG to qemu-img")
    if os.name != "nt":
        pytest.skip("Process I/O counters are available only on Windows")

    sample = Path(sample_value)
    assert sample.stat().st_size <= 16 * 1024 * 1024
    sample_hash = _sha256_file(sample)
    measurements = []

    for run in range(3):
        image = tmp_path / f"{variant}-{run}.{extension}"
        if image_format == "raw":
            shutil.copyfile(sample, image)
        else:
            conversion = subprocess.run(
                [qemu_img, "convert", "-f", "raw", "-O", image_format,
                 "-o", subformat, str(sample), str(image)],
                capture_output=True, text=True, timeout=180)
            assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        assert writer_lib.peare_writer_can_add_files(str(image).encode()) == 1
        opened = peare_lib.open_file(str(image))
        assert opened is not None, variant
        try:
            original_rows = {row["path"]: row["sha256"]
                             for row in opened.normalized_tree()}
        finally:
            opened.close()
        source_path = next(path for path in original_rows
                           if path.count("/") <= 1 and
                           _fat_path_matches(path, "AUTOEXEC.BAT"))
        source_name = source_path.rsplit("/", 1)[-1]
        renamed_name = f"AUTOEXEC.X{run}"
        if run == 0:
            _assert_duplicate_create_is_atomic(
                writer_lib, image, [image], tmp_path, 0, source_name)

        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), 0)
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_rename_entry(
                writer, source_name.encode(), renamed_name.encode())
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            result = writer_lib.peare_writer_update_finish(writer)
            assert result == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, rename_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, rename_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        opened = peare_lib.open_file(str(image))
        assert opened is not None, variant
        try:
            rows = {row["path"]: row["sha256"]
                    for row in opened.normalized_tree()}
            renamed = next(path for path in rows
                           if _fat_path_matches(path, renamed_name))
            assert rows[renamed] == original_rows[source_path]
            assert {path: rows[path] for path in original_rows
                    if path != source_path} == {
                        path: digest for path, digest in original_rows.items()
                        if path != source_path}
            assert source_path not in rows
        finally:
            opened.close()

        raw = image
        if image_format != "raw":
            raw = tmp_path / f"{variant}-{run}-readback.raw"
            conversion = subprocess.run(
                [qemu_img, "convert", "-f", image_format, "-O", "raw",
                 str(image), str(raw)], capture_output=True, text=True,
                timeout=180)
            assert conversion.returncode == 0, conversion.stdout + conversion.stderr
        opened = peare_lib.open_file(str(raw))
        assert opened is not None, variant
        try:
            rows = opened.normalized_tree()
            renamed = next(row for row in rows
                           if _fat_path_matches(row["path"], renamed_name))
            assert renamed["sha256"] == original_rows[source_path]
        finally:
            opened.close()

    print(f"{variant} CUD seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 2.0
    rename_reads = max(row["io_bytes_read_written"][1][0] for row in measurements)
    rename_writes = max(row["io_bytes_read_written"][1][1] for row in measurements)
    if variant in {"raw", "vhd-fixed", "vhd-dynamic", "vhdx-fixed",
                   "vhdx-dynamic", "vmdk-monolithic-sparse"}:
        assert rename_reads < 64 * 1024
        assert rename_writes < 64 * 1024
    else:
        assert rename_reads < sample.stat().st_size * 2
        assert rename_writes < sample.stat().st_size * 2
    assert _sha256_file(sample) == sample_hash

@pytest.mark.parametrize(("sample_env", "filesystem"), [
    ("PEARE_EXFAT_4KN_TEST_IMAGE", "EXFAT"),
    ("PEARE_NTFS_4KN_TEST_IMAGE", "NTFS"),
])
def test_4kn_vhdx_same_directory_rename_latency(
        writer_lib, peare_lib, tmp_path, sample_env, filesystem):
    sample_value = os.environ.get(sample_env, "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip(f"Set {sample_env} to a validated 4Kn VHDX test image")
    sample = Path(sample_value)
    sample_hash = _sha256_file(sample)
    image = tmp_path / sample.name
    shutil.copyfile(sample, image)
    target_marker = "exFAT" if filesystem == "EXFAT" else filesystem
    target_index = _fat_writer_target_index(writer_lib, image, target_marker)
    before_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, filesystem)
    }
    assert before_rows
    source_path = next(
        path for path in before_rows
        if path.rsplit("/", 1)[-1].upper().startswith("R") and
        path.rsplit("/", 1)[-1].upper().endswith(".BIN"))
    current_name = source_path.rsplit("/", 1)[-1]
    measurements = []

    for run in range(3):
        renamed_name = f"L{uuid.uuid4().hex[:6].upper()}.BIN"
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_begin_target(
                writer, str(image).encode(), target_index)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_rename_entry(
                writer, current_name.encode(), renamed_name.encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            rename_seconds = time.perf_counter() - started
            rename_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, rename_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, rename_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)
        current_name = renamed_name

    after_rows = {
        row["path"]: row["sha256"]
        for row in _disk_filesystem_tree(peare_lib, image, filesystem)
    }
    renamed_path = next(
        path for path in after_rows
        if path.rsplit("/", 1)[-1] == current_name)
    assert after_rows[renamed_path] == before_rows[source_path]
    assert {path: after_rows[path] for path in before_rows if path != source_path} == {
        path: digest for path, digest in before_rows.items() if path != source_path
    }
    print(f"4Kn {filesystem} VHDX begin/rename/finish:", measurements)
    image_size = image.stat().st_size
    assert max(row["seconds"][1] for row in measurements) < 2.0
    assert max(row["io_bytes_read_written"][1][0] for row in measurements) < image_size
    assert max(row["io_bytes_read_written"][1][1] for row in measurements) < image_size
    assert max(sum(row["io_bytes_read_written"][2]) for row in measurements) < image_size
    assert _sha256_file(sample) == sample_hash

@pytest.mark.parametrize(("sample_env", "filesystem", "tsk_filesystem"), [
    ("PEARE_NTFS_4KN_DYNAMIC_TEST_IMAGE", "NTFS", "ntfs"),
    ("PEARE_EXFAT_4KN_DYNAMIC_TEST_IMAGE", "exFAT", "exfat"),
], ids=["ntfs", "exfat"])
def test_4kn_large_add_latency_and_sleuthkit_readback(
        writer_lib, tmp_path, sample_env, filesystem, tsk_filesystem):
    sample_value = os.environ.get(sample_env, "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip(f"Set {sample_env} to the local {filesystem} 4Kn VHDX")
    vmware = os.environ.get(
        "PEARE_VMWARE_VDISKMANAGER",
        r"C:\Program Files (x86)\VMware\VMware Workstation\vmware-vdiskmanager.exe")
    tsk_bin = os.environ.get("PEARE_SLEUTHKIT_BIN", "")
    fls = Path(tsk_bin) / "fls.exe"
    icat = Path(tsk_bin) / "icat.exe"
    if not Path(vmware).is_file() or not fls.is_file() or not icat.is_file():
        pytest.skip("VMware Workstation and Sleuth Kit are required for independent read-back")

    sample = Path(sample_value)
    source_hash = _sha256_file(sample)
    payload = tmp_path / "ntfs-large-payload.bin"
    payload.write_bytes(os.urandom(16 * 1024 * 1024))
    payload_hash = _sha256_file(payload).hex()
    measurements = []
    tsk_env = os.environ.copy()
    tsk_env["PATH"] = str(tsk_bin) + os.pathsep + tsk_env.get("PATH", "")

    for run in range(3):
        image = tmp_path / f"ntfs-4kn-large-{run}.vhdx"
        shutil.copyfile(sample, image)
        target_index = _fat_writer_target_index(writer_lib, image, filesystem)
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_files_begin_target(
                writer, str(image).encode(), target_index)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            entry_name = f"PEARE-LARGE-{filesystem.upper()}-{run}.BIN"
            status = writer_lib.peare_writer_add_file(
                writer, str(payload).encode(), entry_name.encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add_seconds = time.perf_counter() - started
            add_io = tuple(after - before for after, before in
                           zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, add_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, add_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        vmdk = tmp_path / f"ntfs-4kn-large-{run}.vmdk"
        converted = subprocess.run(
            [vmware, "-r", str(image), "-t", "0", str(vmdk)],
            capture_output=True, text=True, timeout=180)
        assert converted.returncode == 0, converted.stdout + converted.stderr
        listing = subprocess.run(
            [str(fls), "-f", tsk_filesystem, "-i", "vmdk", str(vmdk)], capture_output=True,
            text=True, timeout=60, env=tsk_env)
        assert listing.returncode == 0, listing.stdout + listing.stderr
        match = re.search(
            rf"(?m)^r/r\s+(\d+)(?:-[^:]*)?:\t{re.escape(entry_name)}\s*$",
            listing.stdout)
        assert match, listing.stdout
        extracted = tmp_path / f"ntfs-4kn-large-{run}-oracle.bin"
        readback = subprocess.run(
            [str(icat), "-f", tsk_filesystem, "-i", "vmdk", str(vmdk),
             match.group(1)],
            capture_output=True, timeout=60, env=tsk_env)
        assert readback.returncode == 0, readback.stderr.decode(errors="replace")
        extracted.write_bytes(readback.stdout)
        assert _sha256_file(extracted).hex() == payload_hash

    print(f"{filesystem} 4Kn VHDX 16 MiB add seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 5.0
    assert max(row["seconds"][2] for row in measurements) < 10.0
    assert _sha256_file(sample) == source_hash

def test_fat32_large_add_latency_and_sleuthkit_readback(
        writer_lib, tmp_path):
    sample_value = os.environ.get("PEARE_FAT32_LARGE_ADD_TEST_IMAGE", "")
    if not sample_value or not Path(sample_value).is_file():
        pytest.skip("Set PEARE_FAT32_LARGE_ADD_TEST_IMAGE to the local FAT32 volume")
    tsk_bin = os.environ.get("PEARE_SLEUTHKIT_BIN", "")
    fls = Path(tsk_bin) / "fls.exe"
    icat = Path(tsk_bin) / "icat.exe"
    if not fls.is_file() or not icat.is_file():
        pytest.skip("Sleuth Kit is required for independent FAT32 read-back")

    sample = Path(sample_value)
    source_hash = _sha256_file(sample)
    payload = tmp_path / "fat32-large-payload.bin"
    payload.write_bytes(os.urandom(16 * 1024 * 1024))
    payload_hash = _sha256_file(payload).hex()
    tsk_env = os.environ.copy()
    tsk_env["PATH"] = str(tsk_bin) + os.pathsep + tsk_env.get("PATH", "")
    measurements = []

    for run in range(3):
        image = tmp_path / f"fat32-large-{run}.img"
        shutil.copyfile(sample, image)
        target_index = _fat_writer_target_index(writer_lib, image, "FAT")
        writer = _HANDLE()
        assert writer_lib.peare_writer_create(C.byref(writer)) == 0
        try:
            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_add_files_begin_target(
                writer, str(image).encode(), target_index)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            begin_seconds = time.perf_counter() - started
            begin_io = tuple(after - before for after, before in
                             zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            entry_name = f"PEARE-FAT32-LARGE-{run}.BIN"
            status = writer_lib.peare_writer_add_file(
                writer, str(payload).encode(), entry_name.encode())
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            add_seconds = time.perf_counter() - started
            add_io = tuple(after - before for after, before in
                           zip(_process_io_counters(), io_before))

            io_before = _process_io_counters()
            started = time.perf_counter()
            status = writer_lib.peare_writer_update_finish(writer)
            assert status == 0, writer_lib.peare_writer_last_error(writer)
            finish_seconds = time.perf_counter() - started
            finish_io = tuple(after - before for after, before in
                              zip(_process_io_counters(), io_before))
            measurements.append({
                "seconds": (begin_seconds, add_seconds, finish_seconds),
                "io_bytes_read_written": (begin_io, add_io, finish_io),
            })
        finally:
            writer_lib.peare_writer_destroy(writer)

        listing = subprocess.run(
            [str(fls), "-f", "fat32", str(image)], capture_output=True,
            text=True, timeout=60, env=tsk_env)
        assert listing.returncode == 0, listing.stdout + listing.stderr
        match = re.search(
            rf"(?m)^r/r\s+(\d+)(?:-[^:]*)?:\t{re.escape(entry_name)}\s*$",
            listing.stdout)
        assert match, listing.stdout
        readback = subprocess.run(
            [str(icat), "-f", "fat32", str(image), match.group(1)],
            capture_output=True, timeout=60, env=tsk_env)
        assert readback.returncode == 0, readback.stderr.decode(errors="replace")
        assert hashlib.sha256(readback.stdout).hexdigest() == payload_hash

    print("FAT32 raw 16 MiB add seconds and I/O:", measurements)
    assert max(row["seconds"][1] for row in measurements) < 5.0
    assert max(row["seconds"][2] for row in measurements) < 10.0
    assert _sha256_file(sample) == source_hash
