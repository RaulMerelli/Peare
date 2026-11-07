"""Check independently generated Intel HEX samples through the Opener ABI."""

import ctypes as C
import hashlib
import json
from pathlib import Path

import pytest
import peare_opener as po


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "formats" / "ihex" / "fixtures"
SAMPLES = (
    ("README-low-address.hex", (0,), None),
    ("README-extended-segment.hex", (0x10000,), 0x10000),
    ("README-extended-linear.hex", (0x100000,), 0x100000),
    ("README-sparse-segments.hex", (0, 0x100000), None),
    ("README-top-address.hex", (0xFFFF0000,), 0xFFFF0000),
)


@pytest.mark.parametrize(("name", "addresses", "start_address"), SAMPLES)
def test_ihex_data_records_match_source(peare_lib, name, addresses, start_address):
    sample = FIXTURES / name
    expected_entries = json.loads(
        (ROOT / "tests" / "formats" / "ihex" / "expected" / (name[:-4] + ".json"))
        .read_text(encoding="utf-8")
    )["tree"]
    expected_entries = [entry for entry in expected_entries if entry["path"].endswith(".bin")]
    opener = peare_lib.open_file(str(sample))
    assert opener is not None
    with opener:
        assert opener.container_format() == "INTEL_HEX"
        data_entries = []
        start_entries = []
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
                    try:
                        kind = context.type_utf8.to_bytes()
                        if kind == b"IHEX_DATA":
                            expected_entry = expected_entries[len(data_entries)]
                            assert context.data_size == expected_entry["size"]
                            address = context.data_offset
                        elif kind == b"IHEX_START_ADDRESS":
                            assert context.data_size == 4
                            start_entries.append(context.identifier_utf8.to_bytes())
                    finally:
                        peare_lib._lib.peare_resource_context_free(C.byref(context))
                    if kind != b"IHEX_DATA":
                        continue
                    blob = po._Blob()
                    assert peare_lib._lib.peare_resource_get_payload(
                        resource, C.byref(blob)
                    ) == 0
                    try:
                        data_entries.append((address, blob.to_bytes()))
                    finally:
                        peare_lib._lib.peare_blob_free(C.byref(blob))
                finally:
                    peare_lib._lib.peare_resource_destroy(resource)
        assert [address for address, _ in data_entries] == list(addresses)
        assert [
            (len(payload), hashlib.sha256(payload).hexdigest())
            for _, payload in data_entries
        ] == [(entry["size"], entry["sha256"]) for entry in expected_entries]
        address_type = "segment" if "segment" in name else "linear"
        expected_start = [] if start_address is None else [
            f"0x{start_address:08x} ({address_type})".encode()
        ]
        assert start_entries == expected_start
