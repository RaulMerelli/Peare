import bz2
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zlib

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _writer_path():
    configured = os.environ.get("PEARE_WRITER_LIB")
    if configured and Path(configured).is_file():
        return Path(configured)
    names = ("PeareWriter.dll", "libPeareWriter.so", "libPeareWriter.dylib")
    for build in ("build-windows/Release", "build-windows", "build/Release", "build"):
        for name in names:
            candidate = ROOT / build / name
            if candidate.is_file():
                return candidate
    return None


@pytest.fixture(scope="module")
def xar_writer():
    path = _writer_path()
    if path is None:
        pytest.skip("PeareWriter library not found")
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(path.resolve().parent))
    library = C.CDLL(str(path))
    handle = C.c_void_p
    library.peare_writer_create.argtypes = [C.POINTER(handle)]
    library.peare_writer_create.restype = C.c_int
    library.peare_writer_destroy.argtypes = [handle]
    library.peare_writer_create_variant_count.argtypes = [C.c_char_p]
    library.peare_writer_create_variant_count.restype = C.c_size_t
    library.peare_writer_create_begin_variant.argtypes = [
        handle, C.c_char_p, C.c_char_p, C.c_char_p]
    library.peare_writer_create_begin_variant.restype = C.c_int
    library.peare_writer_create_file.argtypes = [handle, C.c_char_p, C.c_char_p]
    library.peare_writer_create_file.restype = C.c_int
    library.peare_writer_create_directory.argtypes = [handle, C.c_char_p]
    library.peare_writer_create_directory.restype = C.c_int
    library.peare_writer_can_add_files.argtypes = [C.c_char_p]
    library.peare_writer_can_add_files.restype = C.c_int
    library.peare_writer_update_target_count.argtypes = [C.c_char_p]
    library.peare_writer_update_target_count.restype = C.c_size_t
    library.peare_writer_get_update_capabilities.argtypes = [
        C.c_char_p, C.c_size_t, C.POINTER(C.c_uint32)]
    library.peare_writer_get_update_capabilities.restype = C.c_int
    library.peare_writer_update_begin.argtypes = [handle, C.c_char_p]
    library.peare_writer_update_begin.restype = C.c_int
    library.peare_writer_add_file.argtypes = [handle, C.c_char_p, C.c_char_p]
    library.peare_writer_add_file.restype = C.c_int
    library.peare_writer_replace_file.argtypes = [handle, C.c_char_p, C.c_char_p]
    library.peare_writer_replace_file.restype = C.c_int
    library.peare_writer_create_empty_file.argtypes = [handle, C.c_char_p]
    library.peare_writer_create_empty_file.restype = C.c_int
    library.peare_writer_rename_entry.argtypes = [handle, C.c_char_p, C.c_char_p]
    library.peare_writer_rename_entry.restype = C.c_int
    library.peare_writer_delete_entry.argtypes = [handle, C.c_char_p, C.c_int]
    library.peare_writer_delete_entry.restype = C.c_int
    library.peare_writer_update_finish.argtypes = [handle]
    library.peare_writer_update_finish.restype = C.c_int
    library.peare_writer_finish.argtypes = [handle]
    library.peare_writer_finish.restype = C.c_int
    library.peare_writer_last_error.argtypes = [handle]
    library.peare_writer_last_error.restype = C.c_char_p
    return library


def _xar_payloads(archive):
    magic, header_size, version, compressed_size, expanded_size, checksum_id = \
        struct.unpack_from(">4sHHQQI", archive)
    assert (magic, header_size, version, checksum_id) == (b"xar!", 28, 1, 1)
    toc_end = header_size + compressed_size
    compressed_toc = archive[header_size:toc_end]
    toc = zlib.decompress(compressed_toc)
    assert len(toc) == expanded_size
    heap = archive[toc_end:]
    toc_node = ET.fromstring(toc).find("toc")
    assert toc_node is not None
    checksum = toc_node.find("checksum")
    assert checksum is not None
    assert checksum.attrib["style"].lower() == "sha1"
    assert int(checksum.findtext("offset")) == 0
    digest_size = int(checksum.findtext("size"))
    assert heap[:digest_size] == hashlib.sha1(compressed_toc).digest()

    directories = set()

    def collect(parent, prefix=""):
        found = {}
        for item in parent.findall("file"):
            path = f"{prefix}/{item.findtext('name')}".strip("/")
            if item.findtext("type") == "directory":
                directories.add(path)
                found.update(collect(item, path))
                continue
            if item.findtext("type") == "symlink":
                continue
            data = item.find("data")
            assert data is not None
            offset = int(data.findtext("offset"))
            length = int(data.findtext("length"))
            size = int(data.findtext("size"))
            packed = heap[offset:offset + length]
            encoding = data.find("encoding").attrib["style"]
            if encoding == "application/octet-stream":
                decoded = packed
            elif encoding == "application/x-gzip":
                decoded = zlib.decompress(packed)
            elif encoding == "application/x-bzip2":
                decoded = bz2.decompress(packed)
            else:
                raise AssertionError(f"Unexpected XAR encoding {encoding!r}")
            assert len(decoded) == size
            assert hashlib.sha256(packed).hexdigest() == \
                data.findtext("archived-checksum").strip()
            assert hashlib.sha256(decoded).hexdigest() == \
                data.findtext("extracted-checksum").strip()
            found[path] = decoded
        return found

    return collect(toc_node), directories


@pytest.mark.parametrize("codec", ["store", "gzip", "bzip2"])
def test_xar_writer_round_trip_all_codecs(
        xar_writer, peare_lib, peare_gui_path, tmp_path, codec):
    payload = (b"XAR writer validation\n" * 8192) + bytes(range(256))
    source = tmp_path / "payload.bin"
    source.write_bytes(payload)
    empty = tmp_path / "empty.dat"
    empty.write_bytes(b"")
    output = tmp_path / f"writer-{codec}.xar"
    writer = C.c_void_p()
    assert xar_writer.peare_writer_create(C.byref(writer)) == 0
    try:
        assert xar_writer.peare_writer_create_variant_count(b"xar") == 3
        status = xar_writer.peare_writer_create_begin_variant(
            writer, b"xar", codec.encode(), str(output).encode())
        assert status == 0, xar_writer.peare_writer_last_error(writer).decode()
        assert xar_writer.peare_writer_create_directory(writer, b"docs/nested") == 0
        assert xar_writer.peare_writer_create_directory(writer, b"empty-folder") == 0
        assert xar_writer.peare_writer_create_file(
            writer, str(source).encode(), b"docs/nested/payload.bin") == 0
        assert xar_writer.peare_writer_create_file(
            writer, str(empty).encode(), b"empty.dat") == 0
        status = xar_writer.peare_writer_finish(writer)
        assert status == 0, xar_writer.peare_writer_last_error(writer).decode()
    finally:
        xar_writer.peare_writer_destroy(writer)

    expected = {"docs/nested/payload.bin": payload, "empty.dat": b""}
    files, directories = _xar_payloads(output.read_bytes())
    assert files == expected
    assert directories == {"docs", "docs/nested", "empty-folder"}
    with peare_lib.open_file(str(output)) as opener:
        assert opener.container_format() == "XAR"
        rows = opener.normalized_tree()
    assert {row["path"]: row["sha256"] for row in rows if row["sha256"]} == {
        path: hashlib.sha256(data).hexdigest()
        for path, data in expected.items() if data}
    assert {"docs", "docs/nested", "empty-folder"}.issubset(
        {row["path"] for row in rows})

    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 ("C:/Program Files/7-Zip/7z.exe"
                  if Path("C:/Program Files/7-Zip/7z.exe").is_file() else None))
    if seven_zip:
        result = subprocess.run([seven_zip, "t", str(output)], capture_output=True,
                                text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr

    preview = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
         "--exe", str(peare_gui_path), "--case", f"xar-writer-{codec}",
         str(output), "docs/nested/payload.bin", "--output-dir",
         str(tmp_path / "gui")], capture_output=True, text=True, timeout=120)
    assert preview.returncode == 0, preview.stdout + preview.stderr


def test_xar_real_sample_cud(xar_writer, peare_lib, peare_gui_path, tmp_path):
    sample = ROOT / "tests" / "formats" / "xar" / "fixtures" / "mixed.xar"
    assert sample.is_file()
    source_hash = hashlib.sha256(sample.read_bytes()).digest()
    target = tmp_path / "mixed-cud.xar"
    shutil.copy2(sample, target)
    replacement = ROOT / "README.md"
    added = ROOT / ".gitignore"
    target_bytes = str(target).encode()

    assert xar_writer.peare_writer_can_add_files(target_bytes) == 1
    assert xar_writer.peare_writer_update_target_count(target_bytes) == 1
    capabilities = C.c_uint32()
    assert xar_writer.peare_writer_get_update_capabilities(
        target_bytes, 0, C.byref(capabilities)) == 0
    assert capabilities.value == 0x3F

    writer = C.c_void_p()
    assert xar_writer.peare_writer_create(C.byref(writer)) == 0
    try:
        assert xar_writer.peare_writer_update_begin(writer, target_bytes) == 0
        assert xar_writer.peare_writer_add_file(
            writer, str(added).encode(), b"added/new.txt") == 0
        assert xar_writer.peare_writer_replace_file(
            writer, str(replacement).encode(), b"plain.txt") == 0
        assert xar_writer.peare_writer_create_directory(writer, b"empty/child") == 0
        assert xar_writer.peare_writer_create_empty_file(
            writer, b"empty/child/blank.txt") == 0
        assert xar_writer.peare_writer_rename_entry(
            writer, b"nested", b"renamed") == 0
        assert xar_writer.peare_writer_delete_entry(
            writer, b"setuid.sh", 0) == 0
        assert xar_writer.peare_writer_delete_entry(
            writer, b"empty.txt", 0) == 0
        status = xar_writer.peare_writer_update_finish(writer)
        assert status == 0, xar_writer.peare_writer_last_error(writer).decode()
    finally:
        xar_writer.peare_writer_destroy(writer)

    assert hashlib.sha256(sample.read_bytes()).digest() == source_hash
    files, directories = _xar_payloads(target.read_bytes())
    assert files["plain.txt"] == replacement.read_bytes()
    assert files["added/new.txt"] == added.read_bytes()
    assert files["renamed/deep/file.txt"] == b"nested content here\n"
    assert files["empty/child/blank.txt"] == b""
    assert "setuid.sh" not in files
    assert "empty" in directories and "empty/child" in directories

    tar = shutil.which("tar.exe") or shutil.which("tar")
    assert tar
    listing = subprocess.run([tar, "-tf", str(target)], capture_output=True,
                             text=True, timeout=60)
    assert listing.returncode == 0, listing.stdout + listing.stderr
    assert "alias.lnk" in listing.stdout
    assert "renamed/deep/file.txt" in listing.stdout
    assert "added/new.txt" in listing.stdout
    result = subprocess.run([tar, "-xOf", str(target), "plain.txt"],
                            capture_output=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == replacement.read_bytes()
    result = subprocess.run([tar, "-xOf", str(target), "added/new.txt"],
                            capture_output=True, timeout=60)
    assert result.returncode == 0 and result.stdout == added.read_bytes()
    result = subprocess.run([tar, "-xOf", str(target), "renamed/deep/file.txt"],
                            capture_output=True, timeout=60)
    assert result.returncode == 0 and result.stdout == b"nested content here\n"

    with peare_lib.open_file(str(target)) as opener:
        rows = opener.normalized_tree()
    visible = {row["path"] for row in rows}
    assert {"plain.txt", "added/new.txt", "renamed/deep/file.txt",
            "empty/child/blank.txt"} <= visible
    assert "setuid.sh" not in visible

    for view in ("tree", "details"):
        gui_target = tmp_path / f"mixed-cud-{view}.xar"
        shutil.copy2(sample, gui_target)
        scenario = {
            "view": view,
            "operations": [
                {"type": "create_directory", "name": "gui-dir"},
                {"type": "create_file", "parent": "gui-dir", "name": "blank.txt",
                 "expect_present": "gui-dir/blank.txt"},
                {"type": "replace", "path": "gui-dir/blank.txt",
                 "source": str(replacement), "expect_present": "gui-dir/blank.txt"},
                {"type": "rename", "path": "gui-dir/blank.txt", "name": "renamed.txt",
                 "expect_present": "gui-dir/renamed.txt"},
                {"type": "delete", "path": "gui-dir/renamed.txt",
                 "expect_absent": "gui-dir/renamed.txt"},
                {"type": "replace", "path": "plain.txt",
                 "source": str(replacement), "expect_present": "plain.txt"},
                {"type": "rename", "path": "plain.txt", "name": "renamed.txt",
                 "expect_present": "renamed.txt"},
                {"type": "delete", "path": "renamed.txt",
                 "expect_absent": "renamed.txt"},
                {"type": "add_files", "destination": "gui-dir",
                 "sources": [str(added)], "expect_present": "gui-dir/.gitignore"},
                {"type": "delete", "path": "gui-dir/.gitignore",
                 "expect_absent": "gui-dir/.gitignore"},
                {"type": "delete", "path": "gui-dir", "expect_absent": "gui-dir"},
            ],
        }
        scenario_path = tmp_path / f"xar-gui-{view}.json"
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        output_dir = tmp_path / f"xar-gui-output-{view}"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run_gui_writer_tests.py"),
             "--exe", str(peare_gui_path), "--image", str(gui_target),
             "--scenario", str(scenario_path), "--output-dir", str(output_dir)],
            capture_output=True, text=True, timeout=240)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads((output_dir / "writer-test-report.json").read_text(
            encoding="utf-8"))
        assert all(operation["passed"] for operation in report["operations"])
        checked = subprocess.run([tar, "-tf", str(gui_target)], capture_output=True,
                                 text=True, timeout=60)
        assert checked.returncode == 0, checked.stdout + checked.stderr
