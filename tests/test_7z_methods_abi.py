"""Check 7z method samples through the Opener C ABI against 7-Zip."""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess

import pytest
import peare_opener as po

LOCAL_7Z_ROOT = Path.home() / "Documents" / "inerente a" / "Peare" / "7z-research"
SAMPLE_ROOT = LOCAL_7Z_ROOT / "tool-created-method-samples" / "plain-header-test"
SETTINGS_ROOT = LOCAL_7Z_ROOT / "settings-matrix-complete"
BCJ2_SAMPLE = LOCAL_7Z_ROOT / "tool-created-method-samples" / "bcj2" / "lzma2-bcj2.7z"
MODERN_SAMPLES = LOCAL_7Z_ROOT / "tool-created-method-samples" / "7z-26.05"
SWAP_SAMPLES = ("swap2.7z", "swap4.7z")
AES_SAMPLE = MODERN_SAMPLES / "aes-header.7z"
SPLIT_SAMPLE = MODERN_SAMPLES / "split-test.7z.001"
LARGE_SAMPLE = MODERN_SAMPLES / "large-deflate.7z"
LARGE_SOURCE = Path.home() / "Documents" / "inerente a" / "Peare" / "ArcaOS-5.0.iso"
AES_PASSWORD = "Peare-7z-test"
LEVEL_METHODS = ("lzma2", "lzma", "ppmd", "bzip2")
LEVELS = (0, 1, 3, 5, 7, 9)
SUPPORTED_SAMPLES = (
    ("copy.7z", "Copy"),
    ("deflate.7z", "Deflate"),
    ("bzip2.7z", "BZip2"),
    ("lzma.7z", "LZMA"),
    ("lzma2.7z", "LZMA2"),
    ("ppmd.7z", "PPMD"),
    ("lzma2-bcj.7z", "BCJ"),
    ("lzma2-delta.7z", "Delta"),
    ("lzma2-ppc.7z", "PPC"),
    ("lzma2-ia64.7z", "IA64"),
    ("lzma2-arm.7z", "ARM"),
    ("lzma2-armt.7z", "ARMT"),
    ("lzma2-sparc.7z", "SPARC"),
)
ADDITIONAL_CODECS = (("deflate64.7z", "Deflate64"),)

def _seven_zip():
    configured = os.environ.get("PEARE_7ZIP_EXE")
    candidates = [configured, shutil.which("7z"), shutil.which("7z.exe")]
    candidates.append(LOCAL_7Z_ROOT / "tools" / "7z-26.05" / "extract" /
                      "x64" / "7za.exe")
    candidates.append(str(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
                           "7-Zip" / "7z.exe"))
    return next((path for path in candidates if path and Path(path).is_file()), None)

def _payload_at(peare_lib, opener, folder_index, resource_index):
    resource = po._HANDLE()
    assert peare_lib._lib.peare_opener_open_resource_at(
        opener._h, folder_index, resource_index, po.C.byref(resource)
    ) == 0
    try:
        context = po._Context()
        assert peare_lib._lib.peare_resource_get_context(resource, po.C.byref(context)) == 0
        try:
            assert context.type_utf8.to_bytes().decode("utf-8") == "7Z_FILE"
            size = int(context.data_size)
        finally:
            peare_lib._lib.peare_resource_context_free(po.C.byref(context))
        blob = po._Blob()
        assert peare_lib._lib.peare_resource_get_payload(resource, po.C.byref(blob)) == 0
        try:
            result = blob.to_bytes()
        finally:
            peare_lib._lib.peare_blob_free(po.C.byref(blob))
        assert len(result) == size
        return result
    finally:
        peare_lib._lib.peare_resource_destroy(resource)

def _payload_sha256_at(peare_lib, opener, folder_index, resource_index):
    resource = po._HANDLE()
    assert peare_lib._lib.peare_opener_open_resource_at(
        opener._h, folder_index, resource_index, po.C.byref(resource)
    ) == 0
    try:
        context = po._Context()
        assert peare_lib._lib.peare_resource_get_context(resource, po.C.byref(context)) == 0
        try:
            size = int(context.data_size)
        finally:
            peare_lib._lib.peare_resource_context_free(po.C.byref(context))
        blob = po._Blob()
        assert peare_lib._lib.peare_resource_get_payload(resource, po.C.byref(blob)) == 0
        try:
            assert blob.length == size
            digest = hashlib.sha256()
            offset = 0
            while offset < size:
                count = min(4 * 1024 * 1024, size - offset)
                digest.update(po.C.string_at(blob.bytes + offset, count))
                offset += count
            return size, digest.hexdigest()
        finally:
            peare_lib._lib.peare_blob_free(po.C.byref(blob))
    finally:
        peare_lib._lib.peare_resource_destroy(resource)

def _file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def _resource_index(peare_lib, opener, expected_name):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = po._HANDLE()
            assert peare_lib._lib.peare_opener_open_resource_at(
                opener._h, folder, index, po.C.byref(resource)
            ) == 0
            try:
                context = po._Context()
                assert peare_lib._lib.peare_resource_get_context(
                    resource, po.C.byref(context)
                ) == 0
                try:
                    name = context.identifier_utf8.to_bytes().decode("utf-8")
                finally:
                    peare_lib._lib.peare_resource_context_free(po.C.byref(context))
            finally:
                peare_lib._lib.peare_resource_destroy(resource)
            if name == expected_name:
                return folder, index
    return None

def _assert_archive_members_match_7zip(peare_lib, sample, seven_zip, names):
    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "SEVEN_ZIP"
        for name in names:
            found = _resource_index(peare_lib, opener, name)
            assert found is not None, (sample, name)
            expected = subprocess.run(
                [seven_zip, "x", "-so", str(sample), name],
                check=True, capture_output=True,
            ).stdout
            assert _payload_at(peare_lib, opener, *found) == expected, (sample, name)

@pytest.mark.parametrize("sample_name, expected_method", SUPPORTED_SAMPLES)
def test_existing_7z_codecs_match_7zip_payloads(peare_lib, sample_name, expected_method):
    root = Path(os.environ.get("PEARE_7Z_SAMPLE_ROOT", SAMPLE_ROOT))
    sample = root / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external 7z sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")

    listing = subprocess.run(
        [seven_zip, "l", "-slt", "-ba", str(sample)], check=True,
        capture_output=True, text=True, errors="replace",
    ).stdout
    methods = [line.partition(" = ")[2] for line in listing.splitlines()
               if line.startswith("Method = ")]
    assert methods and any(expected_method.casefold() in
                           [part.casefold() for part in method.replace(" ", ":").split(":")]
                           for method in methods), sample
    expected = subprocess.run(
        [seven_zip, "x", "-so", str(sample)], check=True, capture_output=True
    ).stdout

    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "SEVEN_ZIP"
        assert opener.folder_count() == 1
        assert opener.resource_count(0) == 1
        assert _payload_at(peare_lib, opener, 0, 0) == expected

@pytest.mark.parametrize("sample_name, expected_method", ADDITIONAL_CODECS)
def test_additional_7z_codecs_match_7zip(peare_lib, sample_name, expected_method):
    sample = MODERN_SAMPLES / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external 7-Zip codec sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")
    listing = subprocess.run(
        [seven_zip, "l", "-slt", "-ba", str(sample)], check=True,
        capture_output=True, text=True, errors="replace",
    ).stdout
    assert any(expected_method.casefold() in line.casefold()
               for line in listing.splitlines() if line.startswith("Method = "))
    _assert_archive_members_match_7zip(peare_lib, sample, seven_zip, ("notepad.exe",))

@pytest.mark.parametrize("method", LEVEL_METHODS)
@pytest.mark.parametrize("level", LEVELS)
def test_7z_compression_levels_match_7zip(peare_lib, method, level):
    sample = SETTINGS_ROOT / f"{method}-mx{level}.7z"
    if not sample.is_file():
        pytest.skip(f"Missing external 7z settings sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")
    _assert_archive_members_match_7zip(
        peare_lib, sample, seven_zip, ("README.md", "AGENTS.md")
    )

@pytest.mark.parametrize("sample_name", (
    "lzma2-dict64k-word16.7z", "lzma2-dict4m-word273.7z",
    "lzma-dict64k-word16.7z", "lzma-dict4m-word273.7z",
    "ppmd-mem1m-order2.7z", "ppmd-mem32m-order16.7z",
    "bzip2-block1.7z", "bzip2-block9.7z",
))
def test_7z_coder_properties_match_7zip(peare_lib, sample_name):
    sample = SETTINGS_ROOT / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external 7z property sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")
    _assert_archive_members_match_7zip(peare_lib, sample, seven_zip, ("Qt5Core.dll",))

@pytest.mark.parametrize("solid", ("off", "4m", "on"))
@pytest.mark.parametrize("method", LEVEL_METHODS)
def test_7z_solid_block_settings_match_7zip(peare_lib, method, solid):
    sample = SETTINGS_ROOT / f"{method}-solid-{solid}-large.7z"
    if not sample.is_file():
        pytest.skip(f"Missing external 7z solid sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")
    listing = subprocess.run(
        [seven_zip, "l", "-slt", str(sample)], check=True,
        capture_output=True, text=True, errors="replace",
    ).stdout
    expected_blocks = 1 if solid == "on" else 2
    assert any(line == f"Blocks = {expected_blocks}" for line in listing.splitlines()), sample
    _assert_archive_members_match_7zip(
        peare_lib, sample, seven_zip, ("Qt5Core.dll", "Qt5Widgets.dll")
    )


def test_7z_bcj2_coder_graph_matches_7zip(peare_lib):
    if not BCJ2_SAMPLE.is_file():
        pytest.skip(f"Missing external 7z BCJ2 sample: {BCJ2_SAMPLE}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")
    _assert_archive_members_match_7zip(
        peare_lib, BCJ2_SAMPLE, seven_zip, ("notepad.exe",)
    )

@pytest.mark.parametrize("sample_name", ("lzma2-arm64.7z", "lzma2-riscv.7z"))
def test_7z_modern_branch_filters_match_7zip(peare_lib, sample_name):
    sample = MODERN_SAMPLES / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external 7-Zip 26.05 sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")
    _assert_archive_members_match_7zip(peare_lib, sample, seven_zip, ("notepad.exe",))

@pytest.mark.parametrize("sample_name", SWAP_SAMPLES)
def test_7z_swap_filters_match_7zip(peare_lib, sample_name):
    sample = MODERN_SAMPLES / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external 7-Zip swap-filter sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")
    _assert_archive_members_match_7zip(peare_lib, sample, seven_zip, ("notepad.exe",))

def test_7z_aes_encrypted_header_and_payload_match_7zip(peare_lib):
    if not AES_SAMPLE.is_file():
        pytest.skip(f"Missing external 7-Zip AES sample: {AES_SAMPLE}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")

    missing_password = peare_lib.open_file(str(AES_SAMPLE))
    assert missing_password is None
    assert peare_lib.last_open_status == 14

    wrong_password = peare_lib.open_file(str(AES_SAMPLE), "wrong-password")
    assert wrong_password is None
    assert peare_lib.last_open_status == 15

    opener = peare_lib.open_file(str(AES_SAMPLE), AES_PASSWORD)
    assert opener is not None, peare_lib.status_message(peare_lib.last_open_status)
    expected = subprocess.run(
        [seven_zip, "x", "-so", f"-p{AES_PASSWORD}", str(AES_SAMPLE), "notepad.exe"],
        check=True, capture_output=True,
    ).stdout
    with opener:
        assert opener.container_format() == "SEVEN_ZIP"
        found = _resource_index(peare_lib, opener, "notepad.exe")
        assert found is not None
        assert _payload_at(peare_lib, opener, *found) == expected

def test_7z_split_volumes_match_7zip(peare_lib):
    if not SPLIT_SAMPLE.is_file():
        pytest.skip(f"Missing external split 7-Zip sample: {SPLIT_SAMPLE}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")
    _assert_archive_members_match_7zip(
        peare_lib, SPLIT_SAMPLE, seven_zip, ("notepad.exe",)
    )

def test_7z_deflate_archive_and_payload_exceed_512_mib(peare_lib):
    threshold = 512 * 1024 * 1024
    if not LARGE_SAMPLE.is_file() or not LARGE_SOURCE.is_file():
        pytest.skip("The local large 7z sample or its source ISO is not available")
    assert LARGE_SAMPLE.stat().st_size > threshold
    assert LARGE_SOURCE.stat().st_size > threshold

    opener = peare_lib.open_file(str(LARGE_SAMPLE))
    assert opener is not None, peare_lib.status_message(peare_lib.last_open_status)
    with opener:
        assert opener.container_format() == "SEVEN_ZIP"
        found = _resource_index(peare_lib, opener, "ArcaOS-5.0.iso")
        assert found is not None
        size, digest = _payload_sha256_at(peare_lib, opener, *found)
        assert size == LARGE_SOURCE.stat().st_size
        assert digest == _file_sha256(LARGE_SOURCE)

@pytest.mark.parametrize("sample_name", ("copy-encoded-header.7z", "lzma2-encoded-header.7z"))
def test_encoded_headers_match_7zip_payload(peare_lib, sample_name):
    root = Path(os.environ.get("PEARE_7Z_SAMPLE_ROOT", SAMPLE_ROOT))
    sample = root / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external 7z sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")

    data = sample.read_bytes()
    next_header = 32 + int.from_bytes(data[12:20], "little")
    assert data[next_header] == 0x17
    expected = subprocess.run(
        [seven_zip, "x", "-so", str(sample), r"src\opener\modules\SevenZipModule.cpp"],
        check=True, capture_output=True,
    ).stdout

    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "SEVEN_ZIP"
        found = _resource_index(peare_lib, opener, "SevenZipModule.cpp")
        assert found is not None
        assert _payload_at(peare_lib, opener, *found) == expected

def test_local_solid_7z_corpus_matches_7zip_payloads(peare_lib):
    root_value = os.environ.get("PEARE_7Z_REAL_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_7Z_REAL_SAMPLE_ROOT to run the local real-archive corpus")
    root = Path(root_value)
    archives = sorted(root.glob("*.7z"))
    if not archives:
        pytest.skip(f"No local 7z samples found in {root}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent 7z reference")

    for sample in archives:
        listing = subprocess.run(
            [seven_zip, "l", "-slt", str(sample)], check=True,
            capture_output=True, text=True, errors="replace",
        ).stdout
        ordered = []
        for block in listing.split("\n\n"):
            fields = {line.partition(" = ")[0]: line.partition(" = ")[2]
                      for line in block.splitlines() if " = " in line}
            if fields.get("Path") and "D" not in fields.get("Attributes", "") and \
                    fields.get("Size", "").isdigit():
                ordered.append((fields["Path"].replace("\\", "/"), int(fields["Size"])))

        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample
        with opener:
            assert opener.container_format() == "SEVEN_ZIP"
            resources = {}
            for folder in range(opener.folder_count()):
                parent = opener.folder_type(folder).replace("\x1f", "/")
                for index in range(opener.resource_count(folder)):
                    handle = po._HANDLE()
                    assert peare_lib._lib.peare_opener_open_resource_at(
                        opener._h, folder, index, po.C.byref(handle)
                    ) == 0
                    try:
                        context = po._Context()
                        assert peare_lib._lib.peare_resource_get_context(
                            handle, po.C.byref(context)
                        ) == 0
                        kind = context.type_utf8.to_bytes().decode("utf-8")
                        name = context.identifier_utf8.to_bytes().decode("utf-8")
                        size = int(context.data_size)
                        peare_lib._lib.peare_resource_context_free(po.C.byref(context))
                        if kind == "7Z_FILE":
                            resources[((parent + "/") if parent else "") + name] = (
                                folder, index, size
                            )
                    finally:
                        peare_lib._lib.peare_resource_destroy(handle)
            assert len(ordered) == len(resources), sample
            process = subprocess.Popen(
                [seven_zip, "x", "-so", str(sample)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            try:
                for path, size in ordered:
                    assert path in resources, (sample, path)
                    folder, index, abi_size = resources[path]
                    assert abi_size == size, (sample, path, abi_size, size)
                    payload = _payload_at(peare_lib, opener, folder, index)
                    assert len(payload) == size
                    remaining = size
                    offset = 0
                    while remaining:
                        chunk = process.stdout.read(min(remaining, 1024 * 1024))
                        assert chunk, (sample, path, "7-Zip output ended early")
                        assert payload[offset:offset + len(chunk)] == chunk, (sample, path)
                        offset += len(chunk)
                        remaining -= len(chunk)
                assert not process.stdout.read(1), sample
                stderr = process.stderr.read()
                assert process.wait() == 0, (sample, stderr[-500:])
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
