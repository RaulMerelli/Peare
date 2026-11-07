"""Check official Debian binary packages through the public Opener ABI."""

import os
from pathlib import Path

import pytest


@pytest.mark.parametrize(("sample_name", "hello_sha256"), [
    ("hello_2.1.1-5_amd64.deb",
     "f483ffc7a0ca4c2a44edae25f19b5fc390e178a4d144fa2ea77a3461926bae64"),
    ("hello_2.10-5_amd64.deb",
     "bfda3fef063414535d116a04689caf300568e0691318bcf255f932930508311b"),
    ("hello_2.12.3-1_amd64.deb",
     "5ae9758a83c31370974c8e082688cd0c107e8ca3d712de561bc3309296b2c37e"),
])
def test_debian_package_and_nested_tar_members(peare_lib, sample_name, hello_sha256):
    root = os.environ.get("PEARE_DEB_SAMPLE_ROOT")
    if not root:
        pytest.skip("Set PEARE_DEB_SAMPLE_ROOT to the local Debian sample directory")
    sample = Path(root) / sample_name
    if not sample.is_file():
        pytest.skip(f"Missing official Debian package sample: {sample}")

    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "DEB"
        rows = opener.normalized_tree()

    paths = {row["path"] for row in rows}
    assert "debian-binary" in paths
    assert any(path.startswith("control.tar") for path in paths)
    assert any(path.startswith("data.tar") for path in paths)
    assert any(path.endswith("/control") for path in paths)
    hello = next(row for row in rows if row["path"].endswith("/usr/bin/hello"))
    assert hello["sha256"] == hello_sha256
