import ctypes as C
from pathlib import Path
import shutil
import subprocess

import pytest
import peare_opener as po


SAMPLE = Path.home() / "Desktop" / "ArcaOS-0-s001_JFS_FILE_symbols.zip"


def test_real_zip_implode_members_match_7zip(peare_lib):
    seven_zip = (shutil.which("7z") or shutil.which("7z.exe") or
                 r"C:\Program Files\7-Zip\7z.exe")
    if not SAMPLE.is_file():
        pytest.skip("Local ArcaOS Implode ZIP sample is not available")
    if not Path(seven_zip).is_file():
        pytest.skip("7-Zip is required as an independent ZIP extraction oracle")

    opener = peare_lib.open_file(str(SAMPLE))
    assert opener is not None
    try:
        assert opener.container_format() == "ZIP"
        member_count = 0
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = po._HANDLE()
                assert opener._L.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)
                ) == 0
                try:
                    context = po._Context()
                    assert opener._L.peare_resource_get_context(
                        resource, C.byref(context)
                    ) == 0
                    name = context.identifier_utf8.to_bytes().decode("utf-8")
                    opener._L.peare_resource_context_free(C.byref(context))
                    folder_path = opener.folder_type(folder).replace("\x1f", "/")
                    archive_path = f"{folder_path}/{name}" if folder_path else name
                    payload = po._Blob()
                    assert opener._L.peare_resource_get_payload(
                        resource, C.byref(payload)
                    ) == 0, name
                    try:
                        actual = payload.to_bytes()
                    finally:
                        opener._L.peare_blob_free(C.byref(payload))
                    expected = subprocess.run(
                        [seven_zip, "x", "-so", str(SAMPLE), archive_path],
                        check=True, capture_output=True,
                    ).stdout
                    assert actual == expected, name
                    member_count += 1
                finally:
                    opener._L.peare_resource_destroy(resource)
        assert member_count == 10
    finally:
        opener.close()
