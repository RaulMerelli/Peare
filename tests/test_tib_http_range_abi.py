import ctypes as C
import hashlib
import os
import struct
from urllib.parse import quote

import pytest

from peare_opener import _Context, _HANDLE
from test_callback_source_abi import _HttpTibxRanges, _open_parts
from test_tibx_abi import _open_source_with_password


@pytest.mark.skipif(os.environ.get("PEARE_TIB_ACER_HTTP_RANGE") != "1",
                    reason="Set PEARE_TIB_ACER_HTTP_RANGE=1 to read the public Acer TIB ranges")
def test_legacy_tib_acer_partition_prefix(peare_lib):
    ranges = _HttpTibxRanges(peare_lib, "usb-recovery-acer-e-5-573-w-10.tib",
                            ["USB_Recovery_Acer_E5_573_W10.tib"], [10008935424])
    ranges.chunk = 1024 * 1024
    source = ranges.source(0)
    opener = None
    try:
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and opener is not None, source.error
        assert opener.container_format() == "TIB"
        assert sum(opener.resource_count(i) for i in range(opener.folder_count())) == 1
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            context = _Context()
            assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
            try:
                assert context.identifier_utf8.to_bytes() == b"Partition 1"
                assert context.data_size == 16224616448
            finally:
                opener._L.peare_resource_context_free(C.byref(context))
            prefix = peare_lib.read_resource_original(resource, 0, 4096)
            assert len(prefix) == 4096
            assert hashlib.sha256(prefix).hexdigest() == (
                "a251244647de4b203abf77ef9a217a80d778141f2dbfefd33d23b8e7370857a0")
            nested_source = _HANDLE()
            assert opener._L.peare_resource_get_source(resource, C.byref(nested_source)) == 0
            try:
                volume = peare_lib._open_source(nested_source)
                assert volume is not None
                with volume:
                    data = volume.read_member_bytes("REAGENT.XML")
                    assert len(data) == 1067
                    assert hashlib.sha256(data).hexdigest() == (
                        "8a250ee36cbc294c58a71e2b81888a5fec02b28147644df6e25dbd1b6f737b51")
            finally:
                opener._L.peare_source_destroy(nested_source)
        finally:
            opener._L.peare_resource_destroy(resource)
    finally:
        if opener is not None:
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1 and source.error is None
    assert ranges.transferred <= ranges.limit


@pytest.mark.skipif(os.environ.get("PEARE_TIB_SONY_HTTP_RANGE") != "1",
                    reason="Set PEARE_TIB_SONY_HTTP_RANGE=1 to read the public Sony TIB ranges")
def test_legacy_compressed_tib_sony_first_block(peare_lib):
    name = quote("Sony Vaio NGN-UX380N Recovery Partition.tib")
    ranges = _HttpTibxRanges(peare_lib, "sony-vaio-ngn-ux-380-n-recovery-partition",
                            [name], [6301415705])
    source = ranges.source(0)
    opener = None
    try:
        footer = (C.c_uint8 * 12)()
        received = C.c_size_t()
        assert source.read(None, ranges.sizes[0] - 12, footer, len(footer), C.pointer(received)) == 0
        assert received.value == len(footer)
        assert bytes(footer[8:]) == b"\xb4\x31\x96\x17"
        assert struct.unpack_from("<Q", bytes(footer))[0] == ranges.sizes[0] - 24
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and opener is not None, source.error
        assert opener.container_format() == "TIB"
        assert sum(opener.resource_count(i) for i in range(opener.folder_count())) == 1
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            context = _Context()
            assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
            try:
                assert context.identifier_utf8.to_bytes() == b"Partition 1"
                assert context.data_size == 7433355264
            finally:
                opener._L.peare_resource_context_free(C.byref(context))
            block = peare_lib.read_resource(resource, 0, 262144)
            assert len(block) == 262144
            assert hashlib.sha256(block).hexdigest() == (
                "b4519529c06870d6e667ee6365c785bbae7c895d4f8d72ba2262b9fbd9f6474a"
            )
        finally:
            opener._L.peare_resource_destroy(resource)
    finally:
        if opener is not None:
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1
    assert source.error is None
    assert ranges.transferred <= ranges.limit


@pytest.mark.skipif(os.environ.get("PEARE_TIB_PACKARDBELL_HTTP_RANGE") != "1",
                    reason="Set PEARE_TIB_PACKARDBELL_HTTP_RANGE=1 to read the public split TIB ranges")
def test_split_tib_packardbell_sources(peare_lib):
    ranges = _HttpTibxRanges(peare_lib, "PackardBell_Dot_SE_572RU",
                            [f"pbdot{i}.tib" for i in range(1, 5)],
                            [4284229632, 4284229632, 1437691904, 71170048])
    sources = [ranges.source(i) for i in range(4)]
    opener = None
    try:
        status, unexpected = _open_parts(peare_lib, [sources[0], sources[2], sources[3]])
        if unexpected is not None:
            unexpected.close()
        assert status != 0 and unexpected is None
        status, unexpected = _open_parts(peare_lib, [sources[1], sources[0], sources[2], sources[3]])
        if unexpected is not None:
            unexpected.close()
        assert status != 0 and unexpected is None
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0 and opener is not None, [s.error for s in sources]
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
            source.handle = _HANDLE()
            assert source.released == 0
        assert opener.container_format() == "TIB"
        assert sum(opener.resource_count(i) for i in range(opener.folder_count())) == 1
        resource = _HANDLE()
        assert opener._L.peare_opener_open_resource_at(opener._h, 0, 0, C.byref(resource)) == 0
        try:
            context = _Context()
            assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
            try:
                assert context.identifier_utf8.to_bytes() == b"Partition 1"
                assert context.data_size == 13958643712
            finally:
                opener._L.peare_resource_context_free(C.byref(context))
            block = peare_lib.read_resource(resource, 0, 262144)
            assert len(block) == 262144
            assert hashlib.sha256(block).hexdigest() == (
                "13937841e41ca4b6b2cd3513062d53e3b31d365f1df3d9419017d58f031f45e6"
            )
            for offset, expected in [
                (4642570240, "2e4657687eaf0882aa8b2422d063465e74355de22732a40da5c65f7b3d750682"),
                (9001762816, "5fac29200802ad8c0f080aaebabc5c0df2e2529e947355d8d3d40fa99b44e5e6"),
                (10448535552, "3539473bf70d6db5ae6a62604c3efdff4dd7e57ac0c34df4b6978095b6eb5413"),
            ]:
                cross_volume = peare_lib.read_resource(resource, offset, 262144)
                assert len(cross_volume) == 262144
                assert hashlib.sha256(cross_volume).hexdigest() == expected
        finally:
            opener._L.peare_resource_destroy(resource)
    finally:
        if opener is not None:
            opener.close()
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.error is None and source.released == 1 for source in sources)
    assert ranges.transferred <= ranges.limit
