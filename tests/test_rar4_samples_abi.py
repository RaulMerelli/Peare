"""Check WinRAR-created RAR4 method samples through the public Opener ABI."""

import ctypes as C
import os
from pathlib import Path
import shutil
import subprocess

import pytest
import peare_opener as po


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
    configured = os.environ.get("PEARE_7ZIP_EXE")
    candidates = [configured, shutil.which("7z"), shutil.which("7z.exe")]
    program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
    candidates.append(str(Path(program_files) / "7-Zip" / "7z.exe"))
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
                    finally:
                        peare_lib._lib.peare_resource_context_free(C.byref(context))
                    if name != entry_name:
                        continue
                    blob = po._Blob()
                    assert peare_lib._lib.peare_resource_get_payload(
                        resource, C.byref(blob)
                    ) == 0
                    try:
                        matches.append(blob.to_bytes())
                    finally:
                        peare_lib._lib.peare_blob_free(C.byref(blob))
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
