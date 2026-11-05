"""Check RAR5 members through the public Opener ABI."""

import os
from pathlib import Path
import subprocess

import pytest

from test_rar4_samples_abi import _rar_payload, _seven_zip


@pytest.mark.parametrize("sample_name", (
    "rar5-stored.rar",
    "rar5-m1.rar",
    "rar5-m2.rar",
    "rar5-m3.rar",
    "rar5-m4.rar",
    "rar5-m5.rar",
))
def test_rar5_members_match_7zip(peare_lib, sample_name):
    root_value = os.environ.get("PEARE_RAR4_SAMPLE_ROOT")
    if not root_value:
        pytest.skip("Set PEARE_RAR4_SAMPLE_ROOT to the external archive sample set")
    seven_zip = _seven_zip()
    if not seven_zip:
        pytest.skip("Set PEARE_7ZIP_EXE or install 7-Zip for the independent oracle")
    archive = Path(root_value) / "winrar" / sample_name
    if not archive.is_file():
        pytest.skip(f"Missing external RAR5 sample: {archive.name}")
    oracle = subprocess.run(
        [seven_zip, "x", "-so", str(archive), "CMakeLists.txt"],
        check=True,
        capture_output=True,
    ).stdout
    actual = _rar_payload(peare_lib, archive, "CMakeLists.txt", None, "RAR5")
    assert actual == oracle
