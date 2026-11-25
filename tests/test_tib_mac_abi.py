import os
import zlib
from pathlib import Path

import pytest

from peare_opener import _HANDLE
from test_callback_source_abi import CallbackSource, _open_parts
from test_tibx_abi import _resource_hash


def test_mac_tib_companion_framing_with_original_sample(peare_lib):
    image = os.environ.get("PEARE_TIB_MAC_IMAGE")
    if not image:
        pytest.skip("Set PEARE_TIB_MAC_IMAGE to an authentic Mac TIB and its companion")
    archive = Path(image)
    companion = Path(str(archive) + ".metadata")
    assert archive.is_file() and companion.is_file()
    with archive.open("rb") as f:
        prefix = f.read(32)
        assert len(prefix) == 32 and prefix[:4] == bytes.fromhex("ce24b9a2")
        size = int.from_bytes(prefix[4:6], "little")
        assert size >= 32 and int.from_bytes(prefix[6:8], "little") == 1
        f.seek(0)
        header = bytearray(f.read(size))
    assert len(header) == size
    expected_checksum = int.from_bytes(header[24:28], "little")
    header[24:28] = b"\0" * 4
    assert zlib.adler32(header) == expected_checksum
    with companion.open("rb") as f:
        assert len(f.read(16)) == 16
        record = f.read(4)
        assert len(record) == 4 and record[0] == 0x88
        record_size = int.from_bytes(record[1:4], "little")
        assert record_size >= 10
        packed = f.read(record_size - 4)
        trailer = f.read(4)
    assert len(packed) == record_size - 4 and len(trailer) == 4 and trailer[0] == 0x78
    assert int.from_bytes(trailer[1:4], "little") == record_size
    decoder = zlib.decompressobj()
    decoded = decoder.decompress(packed, 8)
    assert len(decoded) == 8
    box_size = int.from_bytes(decoded[5:8], "little")
    assert box_size != 1
    assert int.from_bytes(decoded[:4], "little") == box_size + 4
    if box_size:
        assert len(decoder.decompress(decoder.unconsumed_tail, box_size)) == box_size
    while decoder.unconsumed_tail:
        decoder.decompress(decoder.unconsumed_tail, 65536)
    assert decoder.eof and not decoder.unused_data

    sources = [CallbackSource(peare_lib, archive), CallbackSource(peare_lib, companion)]
    opener = None
    try:
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0 and opener is not None
        assert all(source.calls > 0 for source in sources)
        offsets = {offset for offset, _ in sources[1].ranges}
        assert 20 in offsets and 16 + record_size in offsets
        expected_path = os.environ.get("PEARE_TIB_MAC_EXPECTED_PATH")
        expected_sha256 = os.environ.get("PEARE_TIB_MAC_EXPECTED_SHA256")
        if not expected_path or not expected_sha256:
            pytest.skip("Set PEARE_TIB_MAC_EXPECTED_PATH and PEARE_TIB_MAC_EXPECTED_SHA256")
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
            source.handle = _HANDLE()
        assert all(source.released == 0 for source in sources)
        assert _resource_hash(opener, expected_path) == expected_sha256.lower()
    finally:
        if opener is not None:
            opener.close()
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)
