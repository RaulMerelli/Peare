"""Compare real CramFS images with independent 7-Zip extractions."""

import hashlib
import os
from pathlib import Path

import pytest


@pytest.mark.parametrize(("sample_env", "oracle_env"), [
    ("PEARE_CRAMFS_LITTLE_SAMPLE", "PEARE_CRAMFS_LITTLE_ORACLE"),
    ("PEARE_CRAMFS_BIG_SAMPLE", "PEARE_CRAMFS_BIG_ORACLE"),
    ("PEARE_CRAMFS_DAHUA_CUSTOM_SAMPLE", "PEARE_CRAMFS_DAHUA_CUSTOM_ORACLE"),
    ("PEARE_CRAMFS_DAHUA_LOGO_SAMPLE", "PEARE_CRAMFS_DAHUA_LOGO_ORACLE"),
    ("PEARE_CRAMFS_CRESTRON_AM100_SAMPLE", "PEARE_CRAMFS_CRESTRON_AM100_ORACLE"),
])
def test_cramfs_members_match_7zip_extraction(peare_lib, sample_env, oracle_env):
    sample_value = os.environ.get(sample_env)
    oracle_value = os.environ.get(oracle_env)
    if not sample_value or not oracle_value:
        pytest.skip(f"Set {sample_env} and {oracle_env} to local CramFS validation files")
    sample = Path(sample_value)
    oracle = Path(oracle_value)
    if not sample.is_file() or not oracle.is_dir():
        pytest.skip("Local CramFS sample or 7-Zip extraction is unavailable")

    expected = {}
    for path in oracle.rglob("*"):
        if path.is_file():
            data = path.read_bytes()
            expected[path.relative_to(oracle).as_posix().lower()] = (
                len(data), hashlib.sha256(data).hexdigest() if data else "")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    with opener:
        assert opener.container_format() == "CRAMFS"
        rows = opener.normalized_tree(
            recurse_containers=True, recurse_empty_containers_only=True,
            include_resource_type=(sample_env == "PEARE_CRAMFS_CRESTRON_AM100_SAMPLE"),
        )

    actual = {
        row["path"].replace("\\", "/").lstrip("/").lower():
        (row["size"], row["sha256"])
        for row in rows if not (row["is_container"] and row["size"] == 0)
    }
    assert expected
    if sample_env == "PEARE_CRAMFS_CRESTRON_AM100_SAMPLE":
        special_resource_types = {
            "CRAMFS_CHARACTER_DEVICE", "CRAMFS_BLOCK_DEVICE",
            "CRAMFS_FIFO", "CRAMFS_SOCKET",
        }
        special_types = {
            row["resource_type"] for row in rows
            if row["resource_type"] in special_resource_types
        }
        assert {
            "CRAMFS_CHARACTER_DEVICE", "CRAMFS_BLOCK_DEVICE"
        } <= special_types
        assert all(
            row["size"] == 0 for row in rows
            if row["resource_type"] in special_resource_types
        )
    assert actual == expected
