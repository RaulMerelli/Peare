"""Open external RPM packages through the public Opener ABI."""

import os
from pathlib import Path

import pytest


def test_rpm_samples_open_and_expose_cpio_members(peare_lib):
    root = os.environ.get("PEARE_RPM_SAMPLE_ROOT")
    if not root:
        pytest.skip("Set PEARE_RPM_SAMPLE_ROOT to the local RPM sample directory")
    sample_root = Path(root)
    samples = sorted(sample_root.rglob("*.rpm"))
    if not samples:
        pytest.skip(f"No RPM samples found under: {sample_root}")

    assert len(samples) >= 75
    for sample in samples:
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample.name
        with opener:
            assert opener.container_format() == "RPM", sample.name
            rows = opener.normalized_tree(max_depth=0)
        if sample.name != "libusbcalls-11.15-1.oc00.i686.rpm":
            assert rows, sample.name


@pytest.mark.parametrize(("sample", "required_path"), [
    ("hello-2.3-1.el3.rf.x86_64.rpm", "usr/bin/hello"),
    ("hello-2.12.1-6.fc42.x86_64.rpm", "usr/bin/hello"),
])
def test_rpm_hello_payload_paths(peare_lib, sample, required_path):
    root = os.environ.get("PEARE_RPM_SAMPLE_ROOT")
    if not root:
        pytest.skip("Set PEARE_RPM_SAMPLE_ROOT to the local RPM sample directory")
    path = Path(root) / sample
    if not path.is_file():
        pytest.skip(f"Missing RPM sample: {path}")

    opener = peare_lib.open_file(str(path))
    assert opener is not None
    with opener:
        rows = opener.normalized_tree()
    paths = {row["path"].replace("\\", "/").lstrip("./") for row in rows}
    assert required_path in paths
    if sample == "hello-2.12.1-6.fc42.x86_64.rpm":
        hello = next(row for row in rows if row["path"].replace("\\", "/").lstrip("./") == required_path)
        assert hello["sha256"] == "8a74f649464f0661ffd103a4a3bf3398311f02b52dff683edf5137b646835e59"
