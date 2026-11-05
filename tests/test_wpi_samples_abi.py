"""Verify real OS/2 WarpIN packages through the public Opener ABI."""

import os
from pathlib import Path

import pytest
import peare_opener as po


DEFAULT_ROOT = Path.home() / "Documents" / "inerente a" / "Peare" / "ArcaOS-5.0"
WPI_SIGNATURE = b"\x77\x04\x02\xBE"


def test_real_wpi_samples_open_and_read_all_payloads(peare_lib):
    root = Path(os.environ.get("PEARE_WPI_SAMPLE_ROOT", DEFAULT_ROOT))
    if not root.is_dir():
        pytest.skip("Set PEARE_WPI_SAMPLE_ROOT to the local ArcaOS sample tree")

    samples = []
    for path in root.rglob("*.WPI"):
        with path.open("rb") as stream:
            if stream.read(4) == WPI_SIGNATURE:
                samples.append(path)
    assert samples, f"No raw WPI packages found under {root}"

    failures = []
    payload_count = 0
    for path in samples:
        opener = peare_lib.open_file(str(path))
        if not opener:
            failures.append(f"{path}: failed to open as WPI")
            continue
        with opener:
            if opener.container_format() != "WPI":
                failures.append(f"{path}: reported format is not WPI")
                continue
            for folder in range(opener.folder_count()):
                for index in range(opener.resource_count(folder)):
                    handle = po._HANDLE()
                    status = peare_lib._lib.peare_opener_open_resource_at(
                        opener._h, folder, index, po.C.byref(handle)
                    )
                    if status != 0:
                        failures.append(f"{path}: resource {folder}/{index} failed to open")
                        continue
                    try:
                        context = po._Context()
                        if peare_lib._lib.peare_resource_get_context(
                            handle, po.C.byref(context)
                        ) != 0:
                            failures.append(f"{path}: resource {folder}/{index} has no context")
                            continue
                        expected_size = int(context.data_size)
                        peare_lib._lib.peare_resource_context_free(po.C.byref(context))
                        blob = po._Blob()
                        if peare_lib._lib.peare_resource_get_payload(
                            handle, po.C.byref(blob)
                        ) != 0:
                            failures.append(f"{path}: resource {folder}/{index} payload failed")
                            continue
                        try:
                            if int(blob.length) != expected_size:
                                failures.append(
                                    f"{path}: resource {folder}/{index} size "
                                    f"{blob.length} != {expected_size}"
                                )
                            else:
                                payload_count += 1
                        finally:
                            peare_lib._lib.peare_blob_free(po.C.byref(blob))
                    finally:
                        peare_lib._lib.peare_resource_destroy(handle)

    assert not failures, "\n".join(failures)
    assert payload_count > 0
