"""Compare local ARJ samples with independent 7-Zip extractions."""

import hashlib
import os
from pathlib import Path

import pytest
import peare_opener as po

REPO = Path(__file__).resolve().parents[1]
SAMPLES = Path(os.environ.get("PEARE_ARJ_SAMPLE_ROOT", REPO / "tmp" / "arj-samples"))
ORACLE = Path(os.environ.get("PEARE_ARJ_ORACLE_ROOT", REPO / "tmp" / "arj-oracle"))
EDGE_SAMPLES = Path(os.environ.get("PEARE_ARJ_EDGE_SAMPLE_ROOT", ""))
ENCRYPTED_SAMPLE = Path(os.environ.get(
    "PEARE_ARJ_ENCRYPTED_SAMPLE", str(EDGE_SAMPLES / "encrypted-sample.arj")
))
ENCRYPTED_ORACLE = Path(os.environ.get("PEARE_ARJ_ENCRYPTED_ORACLE", ""))
GOST40_SAMPLE = Path(os.environ.get("PEARE_ARJ_GOST40_SAMPLE", ""))
GOST40_ORACLE = Path(os.environ.get("PEARE_ARJ_GOST40_ORACLE", ""))
SPLIT_SAMPLE = Path(os.environ.get("PEARE_ARJ_SPLIT_SAMPLE", ""))
SPLIT_ORACLE = Path(os.environ.get("PEARE_ARJ_SPLIT_ORACLE", ""))
GOST256_CASES = [
    ("PEARE_ARJ_GOST256_SAMPLE", "PEARE_ARJ_GOST256_PASSWORD",
     "PEARE_ARJ_GOST256_ORACLE"),
    ("PEARE_ARJ_GOST256L_SAMPLE", "PEARE_ARJ_GOST256L_PASSWORD",
     "PEARE_ARJ_GOST256L_ORACLE"),
    ("PEARE_ARJ_GOST256L64_SAMPLE", "PEARE_ARJ_GOST256L64_PASSWORD",
     "PEARE_ARJ_GOST256L64_ORACLE"),
]


def _hashes(root):
    return {
        path.relative_to(root).as_posix().casefold():
        hashlib.sha256(path.read_bytes()).hexdigest()
        for path in root.rglob("*") if path.is_file()
    }


def test_arj_samples_match_7zip_output(peare_lib):
    samples = sorted(SAMPLES.glob("*.arj")) + sorted(SAMPLES.glob("*.ARJ"))
    samples = list(dict.fromkeys(samples))
    if not samples:
        pytest.skip(f"No local ARJ samples in {SAMPLES}")

    for sample in samples:
        oracle = ORACLE / sample.stem
        if not oracle.is_dir():
            pytest.skip(f"Missing independent 7-Zip extraction for {sample.name}")
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, f"Peare failed to open {sample.name}"
        with opener:
            assert opener.container_format() == "ARJ"
            actual = {
                row["path"].casefold(): row["sha256"]
                for row in opener.snapshot()["tree"]
                if not row["is_container"]
            }
        actual = {Path(path).name.casefold(): digest for path, digest in actual.items()}
        expected = {
            Path(path).name.casefold(): digest
            for path, digest in _hashes(oracle).items()
        }
        assert actual == expected, f"Payload mismatch for {sample.name}"


@pytest.mark.skipif(not os.environ.get("PEARE_ARJ_SPLIT_SAMPLE"),
                    reason="No local ARJ split-volume sample configured")
def test_arj_split_volume_matches_arj32_extraction(peare_lib):
    assert SPLIT_SAMPLE.is_file()
    assert SPLIT_ORACLE.is_file()
    opener = peare_lib.open_file(str(SPLIT_SAMPLE))
    assert opener is not None
    with opener:
        assert opener.container_format() == "ARJ"
        rows = opener.snapshot()["tree"]
        split_file = next(row for row in rows
                          if Path(row["path"]).name.casefold() == SPLIT_ORACLE.name.casefold())
        assert split_file["is_container"]
        assert split_file["size"] == SPLIT_ORACLE.stat().st_size
        resource = po._HANDLE()
        assert peare_lib._lib.peare_opener_open_resource_at(
            opener._h, 0, 0, po.C.byref(resource)
        ) == 0
        try:
            data = peare_lib.read_resource(resource, 0, split_file["size"])
        finally:
            peare_lib._lib.peare_resource_destroy(resource)
    assert data == SPLIT_ORACLE.read_bytes()


@pytest.mark.skipif(not os.environ.get("PEARE_ARJ_EDGE_SAMPLE_ROOT"),
                    reason="No local ARJ encryption and volume samples configured")
def test_arj_legacy_encryption_password_validation(peare_lib):
    assert ENCRYPTED_SAMPLE.is_file()
    assert ENCRYPTED_ORACLE.is_file()
    sample = ENCRYPTED_SAMPLE
    assert peare_lib.open_file(str(sample)) is None
    assert "password" in peare_lib.status_message(peare_lib.last_open_status).casefold()

    opener = peare_lib.open_file(str(sample), password="pearetest")
    assert opener is not None
    with opener:
        rows = opener.snapshot()["tree"]
        extracted = next(row for row in rows
                         if Path(row["path"]).name.casefold() == ENCRYPTED_ORACLE.name.casefold())
        if extracted["is_container"]:
            assert extracted["size"] == ENCRYPTED_ORACLE.stat().st_size
        else:
            assert extracted["sha256"] == hashlib.sha256(ENCRYPTED_ORACLE.read_bytes()).hexdigest()

    opener = peare_lib.open_file(str(sample), password="wrong")
    assert opener is not None
    with opener:
        rows = opener.snapshot()["tree"]
        extracted = next(row for row in rows
                         if Path(row["path"]).name.casefold() == ENCRYPTED_ORACLE.name.casefold())
        assert not extracted["sha256"]


@pytest.mark.skipif(not os.environ.get("PEARE_ARJ_GOST40_SAMPLE"),
                    reason="No local ARJ GOST-40 sample configured")
def test_arj_gost40_encryption_matches_arj32(peare_lib):
    assert GOST40_SAMPLE.is_file()
    assert GOST40_ORACLE.is_file()
    assert peare_lib.open_file(str(GOST40_SAMPLE)) is None

    opener = peare_lib.open_file(str(GOST40_SAMPLE), password="pearetest")
    assert opener is not None
    with opener:
        rows = opener.snapshot()["tree"]
        extracted = next(row for row in rows
                         if Path(row["path"]).name.casefold() == GOST40_ORACLE.name.casefold())
        assert extracted["sha256"] == hashlib.sha256(GOST40_ORACLE.read_bytes()).hexdigest()

    opener = peare_lib.open_file(str(GOST40_SAMPLE), password="wrong")
    assert opener is not None
    with opener:
        rows = opener.snapshot()["tree"]
        extracted = next(row for row in rows
                         if Path(row["path"]).name.casefold() == GOST40_ORACLE.name.casefold())
        assert not extracted["sha256"]


@pytest.mark.parametrize("sample_env,password_env,oracle_env", GOST256_CASES)
def test_arj_gost256_encryption_matches_arj32(peare_lib, sample_env, password_env,
                                              oracle_env):
    sample = Path(os.environ.get(sample_env, ""))
    oracle = Path(os.environ.get(oracle_env, ""))
    password = os.environ.get(password_env, "")
    if not sample.is_file() or not oracle.is_file() or not password:
        pytest.skip(f"No local ARJ sample configured for {sample_env}")

    assert peare_lib.open_file(str(sample)) is None
    opener = peare_lib.open_file(str(sample), password=password)
    assert opener is not None
    with opener:
        rows = opener.snapshot()["tree"]
        extracted = next(row for row in rows
                         if Path(row["path"]).name.casefold() == oracle.name.casefold())
        assert extracted["sha256"] == hashlib.sha256(oracle.read_bytes()).hexdigest()

    opener = peare_lib.open_file(str(sample), password="wrong")
    assert opener is not None
    with opener:
        rows = opener.snapshot()["tree"]
        extracted = next(row for row in rows
                         if Path(row["path"]).name.casefold() == oracle.name.casefold())
        assert not extracted["sha256"]
