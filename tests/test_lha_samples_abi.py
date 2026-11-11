"""Check local, independently sourced LHA samples through the Opener ABI."""

import ctypes as C
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest
import peare_opener as po
from external_tools import enabled, find


LOCAL_PEARE_ROOT = Path.home() / "Documents" / "inerente a" / "Peare"
DEFAULT_ROOT = LOCAL_PEARE_ROOT / "Peare-font-artifacts-20261102" / "peare-font-probes"
SAMPLES = (
    "amiga/Systemfonts1.lha",
    "amiga/Systemfonts2.lha",
    "amiga/Systemfonts3.lha",
    "amiga-all/HKfonts.lha",
    "amiga-all/HKFonts6.lha",
    "amiga-all/mini.font.lha",
    "amiga-all/MoreRussianFonts.lha",
    "amiga-all/RussianFonts-3.lha",
    "amiga-all/RussianFontsKOI8.lha",
    "amiga-all/StarTrekFonts.lha",
    "amiga-all/Systemfonts1.lha",
    "amiga-all/Systemfonts2.lha",
    "amiga-all/Systemfonts3.lha",
    "amiga-all/tracers.font.lha",
    "amiga-all/tyrvas.font.lha",
    "amiga-all/venus.font.lha",
    "amiga-atm/AT1Fonts-C1.lha",
    "amiga-atm/AT1Fonts-I1.lha",
    "amiga-atm/AT1Fonts-SC1.lha",
    "amiga-special/Color1.lha",
    "amiga-special/Color2.lha",
    "amiga-special/DiscFontColly1.lha",
    "amiga-special/IFF-Fonts.lha",
    "tkc/TKC_ColorFonts.lha",
    "tkc/TKC_AnimFonts.lha",
)
ORIGINAL_FIXTURE_ROOT = LOCAL_PEARE_ROOT / "Peare-font-artifacts-20261102" / "lha-original-fixtures" / "orig"
ORIGINAL_FIXTURES = (
    "O_H0.LZH",
    "Z_H0.LZH",
    "Z_H1.LZH",
    "Z_H2.LZH",
    "D_H0.LZH",
    "D_H1.LZH",
    "D_H2.LZH",
)
SEMBIANCE_ROOT = LOCAL_PEARE_ROOT / "LHA-research" / "sembiance-samples"
SEMBIANCE_SAMPLES = (
    "BATS.LZH",
    "LHARK.LZH",
    "MANUAL.LZH",
    "endtheme.lha",
    "example.lha",
    "fanzine9-2.ham.lha",
    "fastcomp.lha",
    "hexify.lha",
    "protracksuportxpk.lha",
)
PMARC_ROOT = LOCAL_PEARE_ROOT / "LHA-research" / "pma-samples"
APACHE_COMMONS_ROOT = LOCAL_PEARE_ROOT / "LHA-research" / "external-samples" / "apache-commons-compress-4f8e097"
APACHE_COMMONS_METHOD_SAMPLES = (
    ("test-amiga-l0-lh4.lha", "-lh4-"),
    ("test-macos-l0-lh6.lha", "-lh6-"),
)
PMARC_SAMPLES = (
    "28may87s.pma",
    "7NIGHTS.PMA",
    "centiped.pma",
    "galaxian.pma",
    "gyruss.pma",
    "qbert.pma",
    "sucobra.pma",
    "unarj.pma",
    "unsqeeze.pma",
    "zipunzip.pma",
)
LHASA_ARCHIVE_ROOT = LOCAL_PEARE_ROOT / "LHA-research" / "lhasa-archives"
LARC_SAMPLES = ("lz4.lzs", "lz5.lzs", "long.lzs", "subdir.lzs")
PMARC1_SAMPLES = ("pm1.pma", "pm1_long.pma", "mtcd.pma")
LZS_ARCHIVES = ("generated/lzs/lzs.lzs", "generated/lzs/long.lzs")
UNLHA32_SAMPLES = ("lh2.lzh", "lh3.lzh", "h2_lhx.lzh", "lhx_long.lzh")
EXTENDED_METHOD_SAMPLES = (
    *((f"larc333/{name}", method) for name, method in zip(
        LARC_SAMPLES, ("-lz4-", "-lz5-", "-lz5-", None)
    )),
    *((name, "-lzs-") for name in LZS_ARCHIVES),
    *((f"pmarc124/{name}", "-pm1-") for name in PMARC1_SAMPLES),
    ("generated/pm1/pm1.pma", "-pm1-"),
    ("pmarc124/pm0.pma", "-pm0-"),
    ("pmarc2/pm0.pma", "-pm0-"),
    ("pmarc2/pm2.pma", "-pm2-"),
)


def _seven_zip():
    if not enabled():
        return None
    configured = os.environ.get("PEARE_7ZIP_EXE")
    candidates = [configured, find("7z", "7z.exe")]
    program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
    candidates.append(str(Path(program_files) / "7-Zip" / "7z.exe"))
    return next((path for path in candidates if path and Path(path).is_file()), None)


def _seven_zip_entries(executable, sample, check=True):
    result = subprocess.run(
        [executable, "l", "-slt", "-ba", str(sample)],
        check=check,
        capture_output=True,
        text=True,
        errors="replace",
    )
    entries = []
    for block in result.stdout.split("\n\n"):
        fields = {}
        for line in block.splitlines():
            key, separator, value = line.partition(" = ")
            if separator:
                fields[key] = value
        if "Path" in fields and "Folder" in fields:
            path = fields["Path"].replace("\\", "/").strip("/")
            if path:
                entries.append((path, int(fields.get("Size", "0")), fields.get("Method", ""), fields["Folder"] == "+"))
    return entries


def _seven_zip_metadata(executable, sample):
    result = subprocess.run(
        [executable, "l", "-slt", "-ba", str(sample)],
        check=True,
        capture_output=True,
        text=True,
        errors="replace",
    )
    entries = {}
    for block in result.stdout.split("\n\n"):
        fields = {}
        for line in block.splitlines():
            key, separator, value = line.partition(" = ")
            if separator:
                fields[key] = value
        if "Path" in fields and "Folder" in fields:
            path = fields["Path"].replace("\\", "/").strip("/")
            if path:
                entries[path] = fields
    return entries


def _lha_crc16(payload):
    crc = 0
    for value in payload:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def _parse_level0_members(archive):
    members = []
    offset = 0
    while offset < len(archive) and archive[offset] != 0:
        header_size = archive[offset]
        header_end = offset + header_size + 2
        assert header_size >= 24 and header_end <= len(archive)
        assert archive[offset + 20] == 0
        assert sum(archive[offset + 2:header_end]) & 0xFF == archive[offset + 1]
        method = archive[offset + 2:offset + 7].decode("ascii")
        packed_size = int.from_bytes(archive[offset + 7:offset + 11], "little")
        size = int.from_bytes(archive[offset + 11:offset + 15], "little")
        name_length = archive[offset + 21]
        name_start = offset + 22
        name = archive[name_start:name_start + name_length].decode("cp437")
        crc_offset = name_start + name_length
        crc = int.from_bytes(archive[crc_offset:crc_offset + 2], "little")
        payload_end = header_end + packed_size
        assert payload_end <= len(archive)
        members.append((method, name.casefold(), size, crc))
        offset = payload_end
    return members


def _peare_entries(peare_lib, opener):
    entries = []
    for folder in range(opener.folder_count()):
        prefix = opener.folder_type(folder).replace("\x1f", "/").replace("\\", "/").strip("/")
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
                    kind = context.type_utf8.to_bytes().decode("utf-8", "replace")
                    name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                    size = int(context.data_size)
                finally:
                    peare_lib._lib.peare_resource_context_free(C.byref(context))
                if kind == "LHA_DIR":
                    continue
                assert kind == "LHA_FILE", (prefix, name, kind)
                path = f"{prefix}/{name}" if prefix else name
                blob = po._Blob()
                assert peare_lib._lib.peare_resource_get_payload(
                    resource, C.byref(blob)
                ) == 0, path
                try:
                    payload = blob.to_bytes()
                finally:
                    peare_lib._lib.peare_blob_free(C.byref(blob))
                assert len(payload) == size, path
                entries.append((path.replace("\\", "/"), payload))
            finally:
                peare_lib._lib.peare_resource_destroy(resource)
    return entries


def test_documented_lha_archives_match_7zip_and_open_through_abi(peare_lib):
    root = Path(os.environ.get("PEARE_LHA_SAMPLE_ROOT", DEFAULT_ROOT))
    samples = [root / relative for relative in SAMPLES]
    if not any(sample.is_file() for sample in samples):
        pytest.skip(f"Missing external LHA sample set: {root}")
    missing = [str(sample) for sample in samples if not sample.is_file()]
    assert not missing, f"Missing documented LHA samples: {missing}"
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent LHA reference")

    methods = set()
    for sample in samples:
        reference_entries = _seven_zip_entries(seven_zip, sample)
        expected_names = [path for path, _, _, directory in reference_entries if not directory]
        methods.update(method for _, _, method, _ in reference_entries)
        expected_payloads = subprocess.run(
            [seven_zip, "x", "-so", str(sample)],
            check=True,
            capture_output=True,
        ).stdout

        expected_entries = []
        payload_offset = 0
        for path, size, _, directory in reference_entries:
            if directory:
                continue
            expected_entries.append((path, expected_payloads[payload_offset:payload_offset + size]))
            payload_offset += size
        assert payload_offset == len(expected_payloads), sample

        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample
        with opener:
            assert opener.container_format() == "LHA"
            actual_entries = _peare_entries(peare_lib, opener)
        assert Counter(path for path, _ in actual_entries) == Counter(expected_names), sample
        assert sorted(actual_entries) == sorted(expected_entries), sample

    assert "-lh0-" in methods
    assert "-lh5-" in methods
    assert "-lhd-" in methods


def test_original_lha_255_fixtures_match_manifest_and_7zip(peare_lib):
    root = Path(os.environ.get("PEARE_LHA_ORIGINAL_FIXTURES_ROOT", ORIGINAL_FIXTURE_ROOT))
    samples = [root / name for name in ORIGINAL_FIXTURES]
    if not any(sample.is_file() for sample in samples):
        pytest.skip(f"Missing external original LHA fixture set: {root}")
    missing = [str(sample) for sample in samples if not sample.is_file()]
    assert not missing, f"Missing documented original LHA fixtures: {missing}"
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent LHA reference")
    manifest = json.loads((root / "expected.json").read_text(encoding="utf-8"))
    methods = set()

    for sample in samples:
        reference_entries = _seven_zip_entries(seven_zip, sample)
        methods.update(method for _, _, method, _ in reference_entries)
        expected_names = [
            Path(path).name for path, _, _, directory in reference_entries if not directory
        ]
        header_level = int(sample.read_bytes()[20])
        expected_level = int(sample.stem[-1])
        assert header_level == expected_level, sample

        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample
        with opener:
            assert opener.container_format() == "LHA"
            actual_entries = _peare_entries(peare_lib, opener)
        if expected_level <= 2:
            assert Counter(Path(path).name for path, _ in actual_entries) == Counter(expected_names), sample
        assert {Path(path).name.casefold() for path, _ in actual_entries} == {
            name.casefold() for name in manifest
        }, sample
        for path, payload in actual_entries:
            expected = next(
                value for name, value in manifest.items()
                if name.casefold() == Path(path).name.casefold()
            )
            assert len(payload) == expected["size"], (sample, path)
            assert hashlib.sha256(payload).hexdigest() == expected["sha256"], (sample, path)
        if sample.name != "O_H0.LZH":
            expected_payloads = subprocess.run(
                [seven_zip, "x", "-so", str(sample)],
                check=True,
                capture_output=True,
            ).stdout
            expected_entries = []
            payload_offset = 0
            for path, size, _, directory in reference_entries:
                if directory:
                    continue
                payload = expected_payloads[payload_offset:payload_offset + size]
                expected_entries.append((path, payload))
                payload_offset += size
            assert payload_offset == len(expected_payloads), sample
            assert sorted((Path(path).name.casefold(), data) for path, data in actual_entries) == sorted(
                (Path(path).name.casefold(), data) for path, data in expected_entries
            ), sample

    assert "-lh0-" in methods
    assert "-lh1-" in methods
    assert "-lh5-" in methods


def test_independent_lha_corpus_matches_7zip_and_opener_abi(peare_lib):
    root = Path(os.environ.get("PEARE_LHA_EXTERNAL_SAMPLE_ROOT", SEMBIANCE_ROOT))
    samples = [root / name for name in SEMBIANCE_SAMPLES]
    if not any(sample.is_file() for sample in samples):
        pytest.skip(f"Missing independent LHA sample set: {root}")
    missing = [str(sample) for sample in samples if not sample.is_file()]
    assert not missing, f"Missing documented LHA samples: {missing}"
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent LHA reference")

    methods = set()
    for sample in samples:
        reference_entries = _seven_zip_entries(seven_zip, sample)
        methods.update(method for _, _, method, _ in reference_entries)
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample
        with opener:
            assert opener.container_format() == "LHA"
            actual_entries = _peare_entries(peare_lib, opener)

        extraction = subprocess.run(
            [seven_zip, "x", "-so", str(sample)],
            capture_output=True,
        )
        if extraction.returncode == 0:
            expected_entries = []
            payload_offset = 0
            for path, size, _, directory in reference_entries:
                if directory:
                    continue
                expected_entries.append(
                    (path, extraction.stdout[payload_offset:payload_offset + size])
                )
                payload_offset += size
            assert payload_offset == len(extraction.stdout), sample
            assert sorted(actual_entries) == sorted(expected_entries), sample
        else:
            metadata = _seven_zip_metadata(seven_zip, sample)
            assert all(method == "-lh7-" for _, _, method, _ in reference_entries), sample
            expected = {
                path.casefold(): (int(item["Size"]), int(item["CRC"], 16))
                for path, item in metadata.items()
                if item.get("Folder") != "+"
            }
            assert {path.casefold() for path, _ in actual_entries} == set(expected), sample
            for path, payload in actual_entries:
                size, crc = expected[path.casefold()]
                assert len(payload) == size, (sample, path)
                assert _lha_crc16(payload) == crc, (sample, path)

    assert {"-lh0-", "-lh5-", "-lh7-"}.issubset(methods)


@pytest.mark.parametrize("sample_name, expected_method", APACHE_COMMONS_METHOD_SAMPLES)
def test_apache_commons_lh4_lh6_fixtures_match_7zip_and_abi(
    peare_lib, sample_name, expected_method
):
    sample = APACHE_COMMONS_ROOT / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external LHA sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent LHA reference")

    reference = [
        item for item in _seven_zip_entries(seven_zip, sample) if not item[3]
    ]
    assert len(reference) == 1, sample
    path, size, method, _ = reference[0]
    assert method == expected_method, sample

    extracted = subprocess.run(
        [seven_zip, "x", "-so", str(sample)], check=True, capture_output=True
    ).stdout
    assert len(extracted) == size, sample

    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "LHA"
        actual = _peare_entries(peare_lib, opener)

    assert actual == [(path, extracted)], sample
    metadata = _seven_zip_metadata(seven_zip, sample)[path]
    assert _lha_crc16(extracted) == int(metadata["CRC"], 16), sample


def test_independent_pmarc_pm2_archives_match_stored_sizes_and_crcs(peare_lib):
    root = Path(os.environ.get("PEARE_PMA_SAMPLE_ROOT", PMARC_ROOT))
    samples = [root / name for name in PMARC_SAMPLES]
    if not any(sample.is_file() for sample in samples):
        pytest.skip(f"Missing independent PMarc sample set: {root}")
    missing = [str(sample) for sample in samples if not sample.is_file()]
    assert not missing, f"Missing documented PMarc samples: {missing}"

    for sample in samples:
        expected = _parse_level0_members(sample.read_bytes())
        assert expected and {method for method, _, _, _ in expected} == {"-pm2-"}, sample
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample
        with opener:
            assert opener.container_format() == "LHA"
            actual = _peare_entries(peare_lib, opener)
        assert {name for _, name, _, _ in expected} == {
            path.casefold() for path, _ in actual
        }, sample
        expected_by_name = {name: (size, crc) for _, name, size, crc in expected}
        for path, payload in actual:
            size, crc = expected_by_name[path.casefold()]
            assert len(payload) == size, (sample, path)
            assert _lha_crc16(payload) == crc, (sample, path)


@pytest.mark.parametrize(
    "relative_path, expected_method",
    EXTENDED_METHOD_SAMPLES,
)
def test_extended_lha_methods_match_header_sizes_and_crcs(
    peare_lib, relative_path, expected_method
):
    sample = LHASA_ARCHIVE_ROOT / relative_path
    if not sample.is_file():
        pytest.skip(f"Missing external LHA sample: {sample}")

    expected = _parse_level0_members(sample.read_bytes())
    assert expected, sample
    if expected_method:
        assert {method for method, _, _, _ in expected} == {expected_method}, sample

    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "LHA"
        actual = _peare_entries(peare_lib, opener)

    expected_by_name = {
        name.replace("\\", "/").casefold(): (size, crc)
        for method, name, size, crc in expected
        if method != "-lhd-"
    }
    assert {path.replace("\\", "/").casefold() for path, _ in actual} == set(expected_by_name), sample
    for path, payload in actual:
        size, crc = expected_by_name[path.replace("\\", "/").casefold()]
        assert len(payload) == size, (sample, path)
        assert _lha_crc16(payload) == crc, (sample, path)


@pytest.mark.parametrize("sample_name", UNLHA32_SAMPLES)
def test_unlha32_lh2_lh3_lhx_samples_match_7zip(peare_lib, sample_name):
    sample = LHASA_ARCHIVE_ROOT / "unlha32" / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing external LHA sample: {sample}")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("7-Zip is required as an independent LHA reference")

    reference = [item for item in _seven_zip_entries(seven_zip, sample) if not item[3]]
    assert reference and all(item[2] in ("-lh2-", "-lh3-", "-lhx-") for item in reference)
    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "LHA"
        actual = _peare_entries(peare_lib, opener)

    expected_names = {path.replace("\\", "/").casefold() for path, _, _, _ in reference}
    actual_by_name = {path.replace("\\", "/").casefold(): data for path, data in actual}
    assert set(actual_by_name) == expected_names, sample

    extraction = subprocess.run([seven_zip, "x", "-so", str(sample)], capture_output=True)
    if extraction.returncode == 0:
        offset = 0
        for path, size, _, _ in reference:
            name = path.replace("\\", "/").casefold()
            expected = extraction.stdout[offset:offset + size]
            assert len(expected) == size, (sample, path)
            assert actual_by_name[name] == expected, (sample, path)
            offset += size
        assert offset == len(extraction.stdout), sample
    else:
        metadata = {
            path.replace("\\", "/").casefold(): fields
            for path, fields in _seven_zip_metadata(seven_zip, sample).items()
        }
        for path, data in actual:
            fields = metadata[path.replace("\\", "/").casefold()]
            assert len(data) == int(fields["Size"]), (sample, path)
            assert _lha_crc16(data) == int(fields["CRC"], 16), (sample, path)


def test_larc_self_extracting_archive_opens_from_embedded_lha(peare_lib):
    sample = LHASA_ARCHIVE_ROOT / "larc333" / "sfx.com"
    if not sample.is_file():
        pytest.skip(f"Missing external LHA sample: {sample}")
    archive = sample.read_bytes()
    header = None
    for offset in range(max(0, len(archive) - 27)):
        if archive[offset + 2:offset + 7] != b"-lz4-" or archive[offset + 20] != 0:
            continue
        header_size = archive[offset]
        end = offset + header_size + 2
        if header_size < 24 or end > len(archive):
            continue
        if sum(archive[offset + 2:end]) & 0xFF != archive[offset + 1]:
            continue
        name_size = archive[offset + 21]
        name_start = offset + 22
        if name_start + name_size + 2 > end:
            continue
        packed_size = int.from_bytes(archive[offset + 7:offset + 11], "little")
        original_size = int.from_bytes(archive[offset + 11:offset + 15], "little")
        crc_start = name_start + name_size
        if crc_start + 2 > end:
            continue
        expected_crc = int.from_bytes(archive[crc_start:crc_start + 2], "little")
        payload = archive[end:end + packed_size]
        if len(payload) == original_size and len(payload) == packed_size:
            header = (archive[name_start:name_start + name_size].decode("latin-1"),
                      expected_crc, payload)
            break
    assert header is not None

    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "LHA"
        actual = _peare_entries(peare_lib, opener)
    assert len(actual) == 1 and actual[0][0].casefold() == header[0].casefold()
    assert actual[0][1] == header[2], sample
    assert _lha_crc16(actual[0][1]) == header[1], sample
