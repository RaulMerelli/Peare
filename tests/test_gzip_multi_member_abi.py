"""Check concatenated GZIP members through the Opener ABI."""

import hashlib
from pathlib import Path

import peare_opener as po


ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / "tests" / "formats" / "gzip" / "fixtures" / "README-plus-docs.tar.gz"
EXPECTED_SIZE = 54211
EXPECTED_SHA256 = "281291583401fd85d44562729fb76f723f9b5ce562d665c06b7258ddcf8ac431"


def test_concatenated_gzip_members_match_source_payloads(peare_lib):
    opener = peare_lib.open_file(str(SAMPLE))
    assert opener is not None
    with opener:
        assert opener.container_format() == "GZIP"
        assert opener.folder_count() == 1
        assert opener.resource_count(0) == 1
        resource = po._HANDLE()
        assert peare_lib._lib.peare_opener_open_resource_at(
            opener._h, 0, 0, po.C.byref(resource)
        ) == 0
        try:
            blob = po._Blob()
            assert peare_lib._lib.peare_resource_get_payload(
                resource, po.C.byref(blob)
            ) == 0
            try:
                payload = blob.to_bytes()
                assert len(payload) == EXPECTED_SIZE
                assert hashlib.sha256(payload).hexdigest() == EXPECTED_SHA256
            finally:
                peare_lib._lib.peare_blob_free(po.C.byref(blob))
        finally:
            peare_lib._lib.peare_resource_destroy(resource)
