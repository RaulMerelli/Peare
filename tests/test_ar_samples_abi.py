"""Test a real GNU ar archive through the public Opener ABI."""

import ctypes as C
import os
from pathlib import Path
import shutil
import subprocess

import pytest
import peare_opener as po


SAMPLE = Path(
    Path.home() / "Documents" / "inerente a" / "Peare" / "Research tools"
    r"\lz4-1.10.0\dll\liblz4.dll.a"
)


def test_gnu_ar_archive_matches_independent_ar(peare_lib, tmp_path):
    sample = Path(os.environ.get("PEARE_AR_SAMPLE", SAMPLE))
    if not sample.is_file():
        pytest.skip("Set PEARE_AR_SAMPLE to a local GNU or BSD ar archive")
    tool = os.environ.get("PEARE_AR_TOOL") or shutil.which("ar")
    if not tool:
        pytest.skip("Set PEARE_AR_TOOL or install ar for the independent oracle")

    expected_names = subprocess.run(
        [tool, "t", str(sample)], check=True, capture_output=True
    ).stdout.decode("utf-8", "replace").splitlines()
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "AR"
        actual_names = []
        payloads = []
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
                    actual_names.append(context.identifier_utf8.to_bytes().decode("utf-8"))
                    peare_lib._lib.peare_resource_context_free(C.byref(context))
                    blob = po._Blob()
                    assert peare_lib._lib.peare_resource_get_payload(
                        resource, C.byref(blob)
                    ) == 0
                    try:
                        payloads.append(blob.to_bytes())
                    finally:
                        peare_lib._lib.peare_blob_free(C.byref(blob))
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)

    assert actual_names == expected_names
    assert len(payloads) == len(expected_names)
    output_dir = tmp_path / "ar-oracle"
    output_dir.mkdir()
    subprocess.run([tool, "x", str(sample)], cwd=output_dir, check=True,
                   capture_output=True)
    for name, payload in zip(actual_names, payloads):
        extracted = output_dir / name
        assert extracted.is_file(), name
        assert payload == extracted.read_bytes(), name
