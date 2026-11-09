import ctypes as C
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _writer_path():
    configured = os.environ.get("PEARE_WRITER_LIB")
    if configured and Path(configured).is_file():
        return Path(configured)
    for build in ("build-windows/Release", "build-windows", "build/Release", "build"):
        for name in ("PeareWriter.dll", "libPeareWriter.so", "libPeareWriter.dylib"):
            candidate = ROOT / build / name
            if candidate.is_file():
                return candidate
    return None


@pytest.fixture(scope="module")
def iso_writer():
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
    library.peare_writer_finish.argtypes = [handle]
    library.peare_writer_finish.restype = C.c_int
    library.peare_writer_last_error.argtypes = [handle]
    library.peare_writer_last_error.restype = C.c_char_p
    return library


def _parse_joliet_tree(image):
    sector_size = 2048
    assert len(image) % sector_size == 0
    pvd = image[16 * sector_size:17 * sector_size]
    svd = image[17 * sector_size:18 * sector_size]
    term = image[18 * sector_size:19 * sector_size]
    assert pvd[0] == 1 and pvd[1:6] == b"CD001" and pvd[6] == 1
    assert svd[0] == 2 and svd[1:6] == b"CD001" and svd[6] == 1
    assert svd[88:91] == b"%/E"
    assert term[0] == 255 and term[1:6] == b"CD001"
    sectors = struct.unpack_from("<I", pvd, 80)[0]
    assert struct.unpack_from(">I", pvd, 84)[0] == sectors
    assert sectors == len(image) // sector_size
    assert struct.unpack_from("<H", pvd, 128)[0] == sector_size
    assert struct.unpack_from(">H", pvd, 130)[0] == sector_size

    path_size = struct.unpack_from("<I", svd, 132)[0]
    path_l = struct.unpack_from("<I", svd, 140)[0]
    path_m = struct.unpack_from(">I", svd, 148)[0]
    tables = []
    for extent, endian in ((path_l, "<"), (path_m, ">")):
        table = image[extent * sector_size:extent * sector_size + path_size]
        assert len(table) == path_size
        rows = []
        offset = 0
        while offset < len(table):
            name_length = table[offset]
            record_size = 8 + name_length + (1 if name_length & 1 else 0)
            extent_value = struct.unpack_from(endian + "I", table, offset + 2)[0]
            parent = struct.unpack_from(endian + "H", table, offset + 6)[0]
            name = table[offset + 8:offset + 8 + name_length]
            rows.append((extent_value, parent, name))
            offset += record_size
        tables.append(rows)
    assert tables[0] == tables[1]
    assert tables[0][0][1:] == (1, b"\0")

    def read_tree(descriptor, joliet):
        root_record = descriptor[156:190]
        root_extent = struct.unpack_from("<I", root_record, 2)[0]
        root_size = struct.unpack_from("<I", root_record, 10)[0]
        files = {}
        directories = set()
        identifiers = {}

        def walk(path, extent, length):
            data = image[extent * sector_size:extent * sector_size + length]
            assert len(data) == length
            offset = 0
            while offset < len(data):
                record_length = data[offset]
                if not record_length:
                    offset = ((offset // sector_size) + 1) * sector_size
                    continue
                record = data[offset:offset + record_length]
                assert len(record) == record_length and record_length >= 34
                name_length = record[32]
                identifier = record[33:33 + name_length]
                if identifier not in (b"\0", b"\1"):
                    raw_name = (identifier.decode("utf-16-be") if joliet
                                else identifier.decode("ascii"))
                    name = raw_name
                    if not record[25] & 2 and name.endswith(";1"):
                        name = name[:-2]
                    child_path = f"{path}/{name}".strip("/")
                    child_extent = struct.unpack_from("<I", record, 2)[0]
                    child_size = struct.unpack_from("<I", record, 10)[0]
                    if record[25] & 2:
                        directories.add(child_path)
                        walk(child_path, child_extent, child_size)
                    else:
                        identifiers[child_path] = raw_name
                        files[child_path] = image[
                            child_extent * sector_size:
                            child_extent * sector_size + child_size]
                        assert len(files[child_path]) == child_size
                offset += record_length

        walk("", root_extent, root_size)
        return files, directories, identifiers

    primary_files, primary_directories, primary_identifiers = read_tree(pvd, False)
    files, directories, _ = read_tree(svd, True)
    return files, directories, tables[0], primary_files, primary_directories, \
        primary_identifiers


def test_iso_joliet_writer_round_trip(iso_writer, peare_lib, peare_gui_path, tmp_path):
    source = ROOT / "README.md"
    payload = source.read_bytes()
    output = tmp_path / "peare-joliet.iso"
    writer = C.c_void_p()
    assert iso_writer.peare_writer_create(C.byref(writer)) == 0
    try:
        assert iso_writer.peare_writer_create_variant_count(b"iso") == 1
        status = iso_writer.peare_writer_create_begin_variant(
            writer, b"iso", b"joliet", str(output).encode())
        assert status == 0, iso_writer.peare_writer_last_error(writer).decode()
        assert iso_writer.peare_writer_create_directory(writer, "docs/日本語".encode()) == 0
        assert iso_writer.peare_writer_create_directory(writer, b"empty") == 0
        assert iso_writer.peare_writer_create_file(
            writer, str(source).encode(), "docs/日本語/README.md".encode()) == 0
        assert iso_writer.peare_writer_create_file(
            writer, str(source).encode(), b"ROOT.TXT") == 0
        assert iso_writer.peare_writer_create_file(
            writer, str(source).encode(), "éA.txt".encode()) == 0
        assert iso_writer.peare_writer_create_file(
            writer, str(source).encode(), "ñA.txt".encode()) == 0
        status = iso_writer.peare_writer_finish(writer)
        assert status == 0, iso_writer.peare_writer_last_error(writer).decode()
    finally:
        iso_writer.peare_writer_destroy(writer)

    (files, directories, path_table, primary_files, primary_directories,
     primary_identifiers) = _parse_joliet_tree(output.read_bytes())
    expected = {
        "docs/日本語/README.md": payload,
        "ROOT.TXT": payload,
        "éA.txt": payload,
        "ñA.txt": payload,
    }
    assert files == expected
    assert directories == {"docs", "docs/日本語", "empty"}
    assert primary_files == {
        "DOCS/___/README.MD": payload,
        "ROOT.TXT": payload,
        "_A.TXT": payload,
        "_A1.TXT": payload,
    }
    assert primary_directories == {"DOCS", "DOCS/___", "EMPTY"}
    assert set(primary_identifiers.values()) == {
        "README.MD;1", "ROOT.TXT;1", "_A.TXT;1", "_A1.TXT;1"
    }
    assert all(len(name.split(";", 1)[0].split(".", 1)[0]) <= 8 and
               ("." not in name.split(";", 1)[0] or
                len(name.split(";", 1)[0].split(".", 1)[1]) <= 3)
               for name in primary_identifiers.values())
    assert [row[2] for row in path_table] == [b"\0", "docs".encode("utf-16-be"),
        "日本語".encode("utf-16-be"), "empty".encode("utf-16-be")]
    assert hashlib.sha256(files["docs/日本語/README.md"]).digest() == \
        hashlib.sha256(payload).digest()

    with peare_lib.open_file(str(output)) as opener:
        rows = opener.normalized_tree()
    visible = {row["path"] for row in rows}
    assert {"docs", "docs/日本語", "docs/日本語/README.md", "ROOT.TXT", "empty"} <= visible

    seven_zip = (shutil.which("7z.exe") or shutil.which("7z") or
                 ("C:/Program Files/7-Zip/7z.exe"
                  if Path("C:/Program Files/7-Zip/7z.exe").is_file() else None))
    if seven_zip:
        result = subprocess.run([seven_zip, "t", str(output)], capture_output=True,
                                text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        listing = subprocess.run([seven_zip, "l", "-slt", str(output)],
                                 capture_output=True, text=True, timeout=60)
        assert listing.returncode == 0, listing.stdout + listing.stderr
        extracted = tmp_path / "seven-zip-extracted"
        result = subprocess.run([seven_zip, "x", "-y", f"-o{extracted}", str(output)],
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        assert (extracted / "docs" / "日本語" / "README.md").read_bytes() == payload
        assert (extracted / "ROOT.TXT").read_bytes() == payload
        assert (extracted / "éA.txt").read_bytes() == payload
        assert (extracted / "ñA.txt").read_bytes() == payload

    preview = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
         "--exe", str(peare_gui_path), "--case", "iso-joliet-writer",
         str(output), "docs/日本語/README.md", "--output-dir",
         str(tmp_path / "gui")], capture_output=True, text=True, timeout=120)
    assert preview.returncode == 0, preview.stdout + preview.stderr
