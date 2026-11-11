"""Check WinRAR-created RAR4 method samples through the public Opener ABI."""

import ctypes as C
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

import pytest
import peare_opener as po
from external_tools import enabled, find


REPO = Path(__file__).resolve().parents[1]
SAMPLES = tuple(
    (f"rar4-m{method}.rar", "ModuleFormat.cpp", None)
    for method in range(6)
) + (
    ("rar4-solid-m3.rar", "ModuleFactory.cpp", None),
    ("rar4-solid-m3.rar", "ModuleFormat.cpp", None),
    ("libarchive/test_read_format_rar_ppmd_lzss_conversion.rar",
     "ppmd_lzss_conversion_test.txt", None),
    ("winrar/rar4-ppmd-m8.rar", "README.md", None),
    ("libarchive/test_read_format_rar_filter.rar", "bsdcat.exe", None),
    ("rar4-encrypted-data.rar", "ModuleFormat.cpp", "PeareRAR4"),
    ("rar4-encrypted-headers.rar", "ModuleFormat.cpp", "PeareRAR4"),
    ("winrar/rar4-multivol-ep.part1.rar", "CMakeLists.txt", None),
    ("winrar/rar4-multivol-old.rar", "CMakeLists.txt", None),
)


def _seven_zip():
    if not enabled():
        return None
    configured = os.environ.get("PEARE_7ZIP_EXE")
    candidates = [configured, find("7z", "7z.exe")]
    program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
    candidates.append(str(Path(program_files) / "7-Zip" / "7z.exe"))
    return next((path for path in candidates if path and Path(path).is_file()), None)


def _winrar_cli():
    if not enabled():
        return None
    configured = os.environ.get("PEARE_WINRAR_CLI_EXE")
    program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
    candidates = [configured, find("Rar.exe"),
                  str(Path(program_files) / "WinRAR" / "Rar.exe")]
    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    candidates.append(str(Path(program_files_x86) / "WinRAR" / "Rar.exe"))
    return next((path for path in candidates if path and Path(path).is_file()), None)


def _rar_payload(peare_lib, archive, entry_name, password, expected_format="RAR4"):
    opener = peare_lib.open_file(str(archive), password=password)
    assert opener is not None, peare_lib.status_message(peare_lib.last_open_status)
    with opener:
        assert opener.container_format() == expected_format
        matches = []
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = po._HANDLE()
                assert peare_lib._lib.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)
                ) == 0
                try:
                    context = po._Context()
                    assert peare_lib._lib.peare_resource_get_context(
                        resource, C.byref(context)
                    ) == 0
                    try:
                        name = context.identifier_utf8.to_bytes().decode(
                            "utf-8", "replace"
                        )
                        size = int(context.data_size)
                    finally:
                        peare_lib._lib.peare_resource_context_free(C.byref(context))
                    if name != entry_name:
                        continue
                    payload = bytearray()
                    offset = 0
                    while offset < size:
                        chunk = peare_lib.read_resource(
                            resource, offset, min(64 * 1024, size - offset)
                        )
                        assert chunk
                        payload.extend(chunk)
                        offset += len(chunk)
                    matches.append(bytes(payload))
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)
        assert len(matches) == 1
        return matches[0]


@pytest.mark.parametrize(("sample_name", "entry_name", "password"), SAMPLES)
def test_rar4_samples_match_7zip(peare_lib, sample_name, entry_name, password):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external WinRAR sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")

    archive = Path(root_value) / sample_name
    if not archive.is_file():
        pytest.skip(f"Missing external RAR4 sample: {archive.name}")

    command = [seven_zip, "x", "-so"]
    if password:
        command.append(f"-p{password}")
    command.extend([str(archive), entry_name])
    oracle = subprocess.run(
        command,
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, entry_name, password)
    if actual != oracle:
        mismatch = next((i for i, pair in enumerate(zip(actual, oracle)) if pair[0] != pair[1]),
                        min(len(actual), len(oracle)))
        pytest.fail(
            f"payload mismatch at byte {mismatch}: actual={len(actual)} oracle={len(oracle)}; "
            f"actual={actual[max(0, mismatch - 8):mismatch + 16].hex()} "
            f"oracle={oracle[max(0, mismatch - 8):mismatch + 16].hex()}"
        )


def test_rar4_encrypted_headers_require_a_valid_password(peare_lib):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external WinRAR sample set")
    archive = Path(root_value) / "rar4-encrypted-headers.rar"
    if not archive.is_file():
        pytest.skip(f"Missing external RAR4 sample: {archive.name}")
    assert peare_lib.open_file(str(archive)) is None
    assert peare_lib.open_file(str(archive), password="incorrect") is None


def test_rar4_compressed_unicode_filename_matches_source(peare_lib):
    archive = REPO / "tests" / "formats" / "rar4" / "rar4-unicode-token.rar"
    expected_name = "café-über-資料.txt"
    actual_payload = _rar_payload(peare_lib, archive, expected_name, None)
    assert len(actual_payload) == 2062
    assert hashlib.sha256(actual_payload).hexdigest() == (
        "84f1b1405488f766cfeae58d72cd7d73aef6d38654bb16887f13ef337fd659a0"
    )


@pytest.mark.parametrize("method", range(1, 6))
def test_rar4_small_solid_ppmd_reader_abi(peare_lib, tmp_path, method):
    rar_cli = _winrar_cli()
    if not rar_cli:
        pytest.skip("WinRAR Rar.exe is required to create the solid PPMd sample")
    first_data = b"small solid stream seed: abcdefghijklmnopqrstuvwxyz\n" * 3
    second_data = b"small solid stream continuation: abcdefghijklmnopqrstuvwxyz\n" * 4
    (tmp_path / "first.txt").write_bytes(first_data)
    (tmp_path / "second.txt").write_bytes(second_data)
    archive = tmp_path / f"solid-ppmd-m{method}.rar"
    subprocess.run([rar_cli, "a", "-ma4", f"-m{method}", "-s", "-ep1",
                    "-mct+", str(archive), "first.txt", "second.txt"],
                   cwd=tmp_path, check=True, capture_output=True, timeout=30)
    subprocess.run([rar_cli, "t", str(archive)], check=True,
                   capture_output=True, timeout=30)

    with peare_lib.open_file(str(archive)) as opener:
        entries = {row["path"]: row for row in
                   opener.normalized_tree(recurse_containers=False)}
    assert entries["first.txt"]["sha256"] == hashlib.sha256(first_data).hexdigest()
    assert entries["second.txt"]["sha256"] == hashlib.sha256(second_data).hexdigest()
