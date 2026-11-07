"""Validate NSIS samples and an official installer through the Opener ABI."""

import hashlib
import json
import os
from collections import Counter
from pathlib import Path

import pytest


DEFAULT_ROOT = (Path.home() / "Documents" / "inerente a" / "Peare" /
                "validation-artifacts" / "nsis")
SAMPLES = tuple(f"sample-{method}-{mode}.exe"
                for method in ("zlib", "bzip2", "lzma")
                for mode in ("nonsolid", "solid"))
EXPECTED_SOURCE_FILES = {
    "README.md": (2062, "84f1b1405488f766cfeae58d72cd7d73aef6d38654bb16887f13ef337fd659a0"),
    "docs/SUPPORTED_FORMATS.md": (13205, "e0482f630081b2d9cb512d9e914bd90b7e9a0295b3807a4341bce2e0df77a1f5"),
}


def _tree_map(rows):
    return {
        row["path"].replace("\\", "/").lstrip("/"):
        (row["size"], row["sha256"])
        for row in rows
    }


@pytest.mark.parametrize("name", SAMPLES)
def test_nsis_compression_samples_match_pinned_payloads(peare_lib, name):
    root = Path(os.environ.get("PEARE_NSIS_SAMPLE_ROOT", DEFAULT_ROOT / "samples"))
    sample = root / name
    if not sample.is_file():
        pytest.skip(f"Missing local NSIS validation sample: {sample}")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "NSIS"
        assert _tree_map(opener.normalized_tree(recurse_containers=False)) == EXPECTED_SOURCE_FILES


def test_official_nsis_installer_matches_7zip_extraction(peare_lib):
    sample_value = os.environ.get("PEARE_NSIS_OFFICIAL_SAMPLE")
    oracle_value = os.environ.get("PEARE_NSIS_7ZIP_ORACLE")
    sample = Path(sample_value) if sample_value else DEFAULT_ROOT / "nsis-3.13-setup.exe"
    oracle = Path(oracle_value) if oracle_value else DEFAULT_ROOT / "oracle-nsis-3.13"
    if not sample.is_file() or not oracle.is_dir():
        pytest.skip("Missing local NSIS 3.13 installer or 7-Zip extraction")

    expected = {}
    for path in oracle.rglob("*"):
        if path.is_file():
            data = path.read_bytes()
            expected[path.relative_to(oracle).as_posix()] = (
                len(data), hashlib.sha256(data).hexdigest() if data else "")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "NSIS"
        actual = _tree_map(opener.normalized_tree(recurse_containers=False))
    assert len(expected) == 445
    assert actual == expected


@pytest.mark.parametrize("compression", ("zlib", "bzip2"))
def test_official_nsis198_compiler_samples_match_pinned_payloads(peare_lib, compression):
    sample = DEFAULT_ROOT / "versions" / f"nsis198-{compression}.exe"
    if not sample.is_file():
        pytest.skip(f"Missing local NSIS 1.98 {compression} validation sample")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "NSIS"
        assert _tree_map(opener.normalized_tree(recurse_containers=False)) == EXPECTED_SOURCE_FILES


def test_official_nsis198_distribution_opens_through_abi(peare_lib):
    sample = DEFAULT_ROOT / "versions" / "nsis198-candidate.exe"
    if not sample.is_file():
        pytest.skip("Missing local NSIS 1.98 distribution installer")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        rows = opener.normalized_tree(recurse_containers=False)
        assert opener.container_format() == "NSIS"
        assert len(rows) == 178
        paths = {row["path"] for row in rows}
        assert "Source/exehead/fileform.h" in paths
        assert "makensis.exe" in paths


def test_official_nsis_release_corpus_matches_pinned_payloads(peare_lib):
    root = Path(os.environ.get("PEARE_NSIS_RELEASE_ROOT", DEFAULT_ROOT / "all-releases"))
    manifest_path = root / "generated-samples.json"
    if not manifest_path.is_file():
        pytest.skip(f"Missing local NSIS release corpus manifest: {manifest_path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    releases = manifest.get("releases", [])
    versions = [release["version"] for release in releases]
    assert len(versions) == len(set(versions))
    assert len(releases) == 71

    expected = {path.rsplit("/", 1)[-1]: value
                for path, value in EXPECTED_SOURCE_FILES.items()}

    for release in releases:
        archive = root / release["archive"]
        assert archive.is_file(), f"Missing NSIS {release['version']} package: {archive}"
        archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
        assert archive_hash == release["archive_sha256"], release["version"]
        sample = root / release["sample"]
        assert sample.is_file(), f"Missing NSIS {release['version']} sample: {sample}"
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, f"Could not open NSIS {release['version']} sample"
        with opener:
            assert opener.container_format() == "NSIS", release["version"]
            actual_rows = opener.normalized_tree(recurse_containers=False)
        actual = _tree_map(actual_rows)
        assert actual == expected, release["version"]


def test_historical_nsis2_release_setups_open_through_abi(peare_lib):
    root = Path(os.environ.get("PEARE_NSIS_HISTORICAL_ROOT", DEFAULT_ROOT))
    manifest_path = root / "historical-setups.json"
    if not manifest_path.is_file():
        pytest.skip(f"Missing local NSIS historical setup manifest: {manifest_path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    releases = [release for release in manifest.get("releases", [])
                if "-" not in release["version"]]
    assert len(releases) == 8
    versions = [release["version"] for release in releases]
    assert len(versions) == len(set(versions))

    for release in releases:
        sample = root / release["sample"]
        assert sample.is_file(), f"Missing NSIS {release['version']} setup: {sample}"
        sample_hash = hashlib.sha256(sample.read_bytes()).hexdigest()
        assert sample_hash == release["sha256"], release["version"]
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, f"Could not open NSIS {release['version']} setup"
        with opener:
            assert opener.container_format() == "NSIS", release["version"]
            actual_rows = opener.normalized_tree(recurse_containers=False)

        oracle = root / "oracle-stable" / f"nsis-{release['version']}"
        assert oracle.is_dir(), f"Missing 7-Zip oracle for NSIS {release['version']}"
        expected = {}
        for path in oracle.rglob("*"):
            if path.is_file() and path.name.casefold() != "uninst-nsis.exe":
                data = path.read_bytes()
                expected[path.relative_to(oracle).as_posix()] = (
                    len(data), hashlib.sha256(data).hexdigest() if data else "")
        assert len(expected) > 0, release["version"]
        actual_payloads = Counter((row["size"], row["sha256"]) for row in actual_rows)
        expected_payloads = set(expected.values())
        assert expected_payloads <= actual_payloads.keys(), release["version"]
        if release["version"] in ("2.0", "2.01", "2.02"):
            actual_paths = {}
            for row in actual_rows:
                path = row["path"].replace("\\", "/").lstrip("/")
                actual_paths.setdefault(path, set()).add((row["size"], row["sha256"]))
            assert actual_paths.keys() == expected.keys(), release["version"]
            assert all(value in actual_paths[path]
                       for path, value in expected.items()), release["version"]
        else:
            assert set(actual_payloads) == expected_payloads, release["version"]


@pytest.mark.parametrize("version", ("2.46", "2.51"))
def test_nsis2_installers_match_7zip_file_payloads(peare_lib, version):
    root = DEFAULT_ROOT / "versions"
    sample = root / f"nsis-{version}-setup.exe"
    oracle = root / f"oracle-nsis-{version}"
    if not sample.is_file() or not oracle.is_dir():
        pytest.skip(f"Missing local NSIS {version} validation data")

    expected = {}
    for path in oracle.rglob("*"):
        if path.is_file() and path.name.casefold() != "uninst-nsis.exe":
            data = path.read_bytes()
            expected[path.relative_to(oracle).as_posix()] = (
                len(data), hashlib.sha256(data).hexdigest() if data else "")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "NSIS"
        rows = opener.normalized_tree(recurse_containers=False)
        actual = _tree_map(rows)
    assert len(expected) == 382
    assert len(rows) == len(expected)
    assert actual == expected
