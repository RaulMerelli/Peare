"""Compare real CHM sample members with independent 7-Zip extractions."""

import hashlib
import os
from pathlib import Path

import pytest


@pytest.mark.parametrize(("sample_env", "oracle_env", "version"), [
    ("PEARE_CHM_WINZIP_SAMPLE", "PEARE_CHM_WINZIP_ORACLE", 3),
    ("PEARE_CHM_WZCLINE_SAMPLE", "PEARE_CHM_WZCLINE_ORACLE", 3),
    ("PEARE_CHM_ITSF2_SAMPLE", "PEARE_CHM_ITSF2_ORACLE", 2),
])
def test_chm_members_match_7zip_extraction(peare_lib, sample_env, oracle_env, version):
    sample_value = os.environ.get(sample_env)
    oracle_value = os.environ.get(oracle_env)
    if not sample_value or not oracle_value:
        pytest.skip(f"Set {sample_env} and {oracle_env} to local CHM validation files")
    sample = Path(sample_value)
    oracle = Path(oracle_value)
    if not sample.is_file() or not oracle.is_dir():
        pytest.skip("Local CHM sample or 7-Zip extraction is unavailable")
    with sample.open("rb") as stream:
        header = stream.read(12)
    assert header[:4] == b"ITSF"
    assert int.from_bytes(header[4:8], "little") == version

    expected = {}
    for path in oracle.rglob("*"):
        if path.is_file():
            data = path.read_bytes()
            expected[path.relative_to(oracle).as_posix().lower()] = (
                len(data), hashlib.sha256(data).hexdigest() if data else "")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "CHM"
        rows = opener.normalized_tree(max_depth=0)

    actual = {
        row["path"].replace("\\", "/").lstrip("/").lower():
        (row["size"], row["sha256"])
        for row in rows
    }
    assert len(expected) >= 60
    for path, value in expected.items():
        assert actual.get(path) == value, f"Member mismatch: {path}"


def test_early_itsf_v2_sample_opens_and_reads_members(peare_lib):
    sample_value = os.environ.get("PEARE_CHM_ITSF2_SAMPLE")
    if not sample_value:
        pytest.skip("Set PEARE_CHM_ITSF2_SAMPLE to the local IE4 Preview 2 CHM sample")
    sample = Path(sample_value)
    if not sample.is_file():
        pytest.skip("Local ITSF v2 CHM sample is unavailable")
    with sample.open("rb") as stream:
        header = stream.read(12)
    assert header[:4] == b"ITSF"
    assert int.from_bytes(header[4:8], "little") == 2
    assert int.from_bytes(header[8:12], "little") == 0x58

    expected = {
        "#system": (69, "0c861c5476d844da399275dd79b295a93eb6824a61f9f43f8ee11a5624a7ab48"),
        "mswallet.hhc": (577, "271ae499bad5ba2b00e45eab120f8ce67cb3a4cfd585c46beca452ebbe5b03b8"),
        "over0khj.htm": (3273, "df59ed2d28bdf7b0d3f7ba0c1e80763dd78a1466e25bcfb0e1bf058a60289112"),
        "over79h0.htm": (1792, "ae11ce961be3c229a7d5838f97cff2117a869cf05cac82fd74e37f40ca5558ca"),
    }
    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "CHM"
        rows = opener.normalized_tree(max_depth=0)

    actual = {
        row["path"].replace("\\", "/").lstrip("/").lower():
        (row["size"], row["sha256"])
        for row in rows
    }
    assert actual == expected
