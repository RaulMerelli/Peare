import ctypes as C
import hashlib
import os
import struct
import threading
import urllib.request
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from peare_opener import Opener, _Context, _HANDLE
from test_tibx_abi import _directory_source, _open_source_with_password, _resource_hash


SAMPLES = os.environ.get("PEARE_ACRONIS_SAMPLES")
requires_samples = pytest.mark.skipif(not SAMPLES, reason="Set PEARE_ACRONIS_SAMPLES")
TAR_SAMPLE = Path(__file__).resolve().parent / "formats" / "tar" / "fixtures" / "docs.tar"

READ = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.POINTER(C.c_uint8),
                   C.c_size_t, C.POINTER(C.c_size_t))
RELEASE = C.CFUNCTYPE(None, C.c_void_p)


class CallbackSource:
    def __init__(self, lib, path, mode=None, name=None):
        self.lib = lib
        self.file = path.open("rb")
        self.size = path.stat().st_size
        self.lock = threading.Lock()
        self.mode = mode
        self.released = 0
        self.calls = 0
        self.ranges = []
        self.reader = READ(self.read)
        self.release = RELEASE(self.close)
        create = lib._lib.peare_source_create_callback
        create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, READ, RELEASE,
                           C.POINTER(_HANDLE)]
        create.restype = C.c_int
        self.handle = _HANDLE()
        status = create(self.size, (name or path.name).encode("utf-8"), None,
                        self.reader, self.release, C.byref(self.handle))
        assert status == 0

    def read(self, context, offset, buffer, capacity, out_read):
        with self.lock:
            self.calls += 1
            self.ranges.append((offset, capacity))
            if self.mode == "error":
                return 13
            if self.mode == "zero":
                out_read[0] = 0
                return 0
            if self.mode == "overcount":
                out_read[0] = capacity + 1
                return 0
            if self.mode == "partial_then_error" and self.calls > 1:
                return 13
            self.file.seek(offset)
            data = self.file.read(min(capacity, 1 if self.mode == "partial_then_error" else 127))
            C.memmove(buffer, data, len(data))
            out_read[0] = len(data)
            return 0

    def close(self, context):
        self.released += 1
        self.file.close()


def _find_resource(opener, name):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(opener._h, folder, index, C.byref(resource)) == 0
            context = _Context()
            assert opener._L.peare_resource_get_context(resource, C.byref(context)) == 0
            try:
                found = context.identifier_utf8.to_bytes().decode("utf-8") == name
                size = int(context.data_size)
            finally:
                opener._L.peare_resource_context_free(C.byref(context))
            if found:
                return resource, size
            opener._L.peare_resource_destroy(resource)
    raise AssertionError("Resource not found: " + name)


def _open_parts(peare_lib, sources, password=None):
    lib = peare_lib._lib
    lib.peare_opener_open_parts.argtypes = [_HANDLE, C.POINTER(_HANDLE), C.c_size_t]
    lib.peare_opener_open_parts.restype = C.c_int
    opener = _HANDLE()
    assert lib.peare_opener_create(C.byref(opener)) == 0
    if password is not None:
        assert lib.peare_opener_set_password(opener, password.encode("utf-8")) == 0
    handles = (_HANDLE * len(sources))(*(source.handle for source in sources))
    status = lib.peare_opener_open_parts(opener, handles, len(sources))
    if status:
        lib.peare_opener_destroy(opener)
        return status, None
    return status, Opener(peare_lib, opener)


def test_callback_parts_reject_empty_and_unsupported(peare_lib):
    status, opener = _open_parts(peare_lib, [])
    assert status == 1 and opener is None
    source = CallbackSource(peare_lib, TAR_SAMPLE)
    try:
        status, opener = _open_parts(peare_lib, [source, source])
        if opener is not None:
            opener.close()
        assert status == 6 and opener is None
    finally:
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@requires_samples
def test_acronis15_incremental_file_callback_parts(peare_lib):
    directory = Path(SAMPLES) / "acronis15-created" / "legacy-incremental"
    paths = [directory / "legacy_2026_11_14_20_26_57_651F.TIB",
             directory / "legacy_2026_11_14_20_26_57_651F2.TIB"]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 incremental backup not available")
    sources = [CallbackSource(peare_lib, path) for path in paths]
    opener = None
    try:
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0 and opener is not None
        assert _resource_hash(opener, "C:/PeareInput/sample.txt") == (
            "931b99a0455cb56c50642cada2e5048efda927f8bb4e861a1ed9c0d2b67ce6ff"
        )
    finally:
        if opener is not None:
            opener.close()
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@requires_samples
@pytest.mark.parametrize(("record_offset", "record_tag"), [(280, "e"), (1337, "e"), (2389, "g")])
def test_acronis15_incremental_corrupt_index_fails(peare_lib, record_offset, record_tag):
    directory = Path(SAMPLES) / "acronis15-created" / "legacy-incremental"
    paths = [directory / "legacy_2026_11_14_20_26_57_651F.TIB",
             directory / "legacy_2026_11_14_20_26_57_651F2.TIB"]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 incremental backup not available")
    assert paths[1].read_bytes()[record_offset] == ord(record_tag)

    class CorruptIndexSource(CallbackSource):
        changed = False

        def read(self, context, offset, buffer, capacity, out_read):
            status = super().read(context, offset, buffer, capacity, out_read)
            if status == 0 and offset <= record_offset + 1 < offset + out_read[0]:
                buffer[record_offset + 1 - offset] ^= 1
                self.changed = True
            return status

    sources = [CallbackSource(peare_lib, paths[0]), CorruptIndexSource(peare_lib, paths[1])]
    opener = None
    try:
        status, opener = _open_parts(peare_lib, sources)
        assert sources[1].changed
        assert status != 0 and opener is None
    finally:
        if opener is not None:
            opener.close()
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@requires_samples
@pytest.mark.parametrize("order", [(0, 1, 2), (0, 2, 1)])
def test_acronis15_split_file_callback_parts(peare_lib, order):
    from test_tib_abi import ACRONIS15_SPLIT

    directory = Path(SAMPLES) / "acronis15-created"
    paths = [directory / name for name, _ in ACRONIS15_SPLIT]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 split file backup not available")
    sources = [CallbackSource(peare_lib, path) for path in paths]
    opener = None
    try:
        status, opener = _open_parts(peare_lib, [sources[i] for i in order])
        if order != (0, 1, 2):
            assert status != 0 and opener is None
        else:
            assert status == 0 and opener is not None
            for source in sources:
                peare_lib._lib.peare_source_destroy(source.handle)
                source.handle = _HANDLE()
                assert source.released == 0
            assert _resource_hash(opener, "C:/Windows/System32/ntdll.dll") == (
                "43a5a732667cad04776a1ccf51e152c48705ad3ba486e0c628b7386e9a7138d7"
            )
    finally:
        if opener is not None:
            opener.close()
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@requires_samples
@pytest.mark.parametrize("password", ["incorrect", "PeareTest2026"])
def test_acronis15_split_encrypted_callback_parts(peare_lib, password):
    from test_tib_abi import ACRONIS15_SPLIT_AES

    directory = Path(SAMPLES) / "acronis15-created"
    paths = [directory / name for name, _ in ACRONIS15_SPLIT_AES]
    if not all(path.is_file() for path in paths):
        pytest.skip("Acronis Cyber Protect 15 encrypted split backup not available")
    sources = [CallbackSource(peare_lib, path) for path in paths]
    opener = None
    try:
        status, opener = _open_parts(peare_lib, sources, password)
        if password == "incorrect":
            assert status == 15 and opener is None
        else:
            assert status == 0 and opener is not None
            assert _resource_hash(opener, "C:/Windows/System32/ntdll.dll", password) == (
                "43a5a732667cad04776a1ccf51e152c48705ad3ba486e0c628b7386e9a7138d7"
            )
    finally:
        if opener is not None:
            opener.close()
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@requires_samples
def test_tibx_callback_parts_single_source(peare_lib):
    source = CallbackSource(peare_lib, Path(SAMPLES) / "t1_simple_ntfs.tibx")
    opener = None
    try:
        status, opener = _open_parts(peare_lib, [source])
        assert status == 0 and opener is not None
        peare_lib._lib.peare_source_destroy(source.handle)
        source.handle = _HANDLE()
        assert source.released == 0
        assert _resource_hash(opener, "disk1/part1", ranges=[(0, 4096)]) == [
            "1da54088530e349d304e4b64a2ec3aee93046eeb78b9b208ae4ef5fa6320d0d6"
        ]
    finally:
        if opener is not None:
            opener.close()
        if source.handle:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@requires_samples
def test_tib_raw_map_read_failure_is_reported(peare_lib):
    class FailingMapSource(CallbackSource):
        failed = False

        def read(self, context, offset, buffer, capacity, out_read):
            if offset <= 3357174 < offset + capacity:
                self.failed = True
                out_read[0] = 0
                return 13
            return super().read(context, offset, buffer, capacity, out_read)

    sample = Path(SAMPLES) / "lte-elite-hdd.tib"
    digest = hashlib.sha1()
    with sample.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    assert digest.hexdigest() == "5b1875c1eb093eaabab7e63e28028fb6db1d38a7"
    source = FailingMapSource(peare_lib, sample)
    opener = None
    try:
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        assert source.failed
        assert status != 0 and opener is None
    finally:
        if opener is not None:
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@requires_samples
def test_tibx_callback_parts_lifetime_and_bytes(peare_lib):
    sources = [CallbackSource(peare_lib, Path(SAMPLES) / name) for name in
               ("t1_chain_diff.tibx", "t1_chain_diff-0001.tibx")]
    opener = None
    try:
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0 and opener is not None
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
            source.handle = _HANDLE()
            assert source.released == 0
        nested = _directory_source(opener, "4")
        opener.close()
        opener = None
        assert all(source.released == 0 for source in sources)
        try:
            status, opener = _open_source_with_password(peare_lib, nested, None)
            assert status == 0 and opener is not None
        finally:
            peare_lib._lib.peare_source_destroy(nested)
        assert _resource_hash(opener, "disk1/part1", ranges=[(0, 4096)]) == [
            "125c796821142a764b3213218e240f9a7ce679934b7bdb03fdf6f419dd6a42e4"
        ]
        assert all(source.calls for source in sources)
    finally:
        if opener is not None:
            opener.close()
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@requires_samples
@pytest.mark.parametrize("order", [(1, 0), (0, 0)])
def test_tibx_callback_parts_reject_ambiguous_order(peare_lib, order):
    sources = [CallbackSource(peare_lib, Path(SAMPLES) / name) for name in
               ("t1_chain_diff.tibx", "t1_chain_diff-0001.tibx")]
    try:
        status, opener = _open_parts(peare_lib, [sources[i] for i in order])
        if opener is not None:
            opener.close()
        assert status != 0 and opener is None
    finally:
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@requires_samples
def test_tibx_callback_parts_reject_id_not_in_archive_table(peare_lib):
    sources = [CallbackSource(peare_lib, Path(SAMPLES) / "t1_chain_diff.tibx"),
               CallbackSource(peare_lib, Path(SAMPLES) / "t1_chain_diff-0001.tibx",
                              name="t1_chain_diff-0002.tibx")]
    try:
        status, opener = _open_parts(peare_lib, sources)
        if opener is not None:
            opener.close()
        assert status != 0 and opener is None
        assert sources[1].calls > 0
    finally:
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@requires_samples
def test_tibx_callback_parts_propagate_io_error(peare_lib):
    sources = [CallbackSource(peare_lib, Path(SAMPLES) / "t1_chain_diff.tibx"),
               CallbackSource(peare_lib, Path(SAMPLES) / "t1_chain_diff-0001.tibx", "error")]
    try:
        status, opener = _open_parts(peare_lib, sources)
        if opener is not None:
            opener.close()
        assert status == 13 and opener is None
        assert sources[1].calls > 0
    finally:
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@requires_samples
def test_tibx_callback_parts_late_io_invalidates_resource(peare_lib):
    sources = [CallbackSource(peare_lib, Path(SAMPLES) / name) for name in
               ("t1_chain_diff.tibx", "t1_chain_diff-0001.tibx")]
    openers, resource = [], _HANDLE()
    try:
        status, root = _open_parts(peare_lib, sources)
        assert status == 0 and root is not None
        openers.append(root)
        for name in ("4", "disk1"):
            nested_source = _directory_source(openers[-1], name)
            try:
                status, child = _open_source_with_password(peare_lib, nested_source, None)
                assert status == 0 and child is not None
                openers.append(child)
            finally:
                peare_lib._lib.peare_source_destroy(nested_source)
        resource, size = _find_resource(openers[-1], "part1")
        sources[1].mode = "error"
        before = sources[1].calls
        buffer = (C.c_uint8 * 32)()
        for offset in range(0, size, 1024 * 1024):
            count = C.c_size_t(99)
            status = peare_lib._lib.peare_resource_read(resource, offset, buffer, len(buffer), C.byref(count))
            if sources[1].calls > before:
                break
            assert status == 0
        assert sources[1].calls > before
        assert status == 13 and count.value == 0
        count = C.c_size_t(99)
        assert peare_lib._lib.peare_resource_read(resource, 0, buffer, len(buffer), C.byref(count)) == 13
        assert count.value == 0
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        for opener in reversed(openers):
            opener.close()
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


class _HttpTibxRanges:
    def __init__(self, peare_lib, item, names, sizes):
        self.lib = peare_lib
        self.item = item
        self.sizes = sizes
        self.names = names
        self.lock = threading.RLock()
        self.cache = OrderedDict()
        self.transferred = 0
        self.touched = set()
        self.limit, self.chunk = 16 * 1024 * 1024, 32 * 1024

    def source(self, index):
        ranges = self

        class RangeSource:
            def __init__(self):
                self.index = index
                self.handle = _HANDLE()
                self.released = 0
                self.error = None
                self.reader = READ(self.read)
                self.release = RELEASE(self.close)
                create = ranges.lib._lib.peare_source_create_callback
                create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, READ, RELEASE, C.POINTER(_HANDLE)]
                create.restype = C.c_int
                assert create(ranges.sizes[index], ranges.names[index].encode(), None, self.reader, self.release,
                              C.byref(self.handle)) == 0

            def read(self, context, offset, buffer, capacity, out_read):
                out_read[0] = 0
                try:
                    with ranges.lock:
                        start = (offset // ranges.chunk) * ranges.chunk
                        length = min(ranges.chunk, ranges.sizes[index] - start)
                        key = (index, start)
                        if key not in ranges.cache:
                            if ranges.transferred + length > ranges.limit:
                                raise RuntimeError("HTTP range budget exceeded")
                            end = start + length - 1
                            request = urllib.request.Request(
                                "https://archive.org/download/" + ranges.item + "/" + ranges.names[index],
                                headers={"Range": f"bytes={start}-{end}", "Accept-Encoding": "identity"})
                            ranges.transferred += length
                            with urllib.request.urlopen(request, timeout=30) as response:
                                if response.status != 206 or response.headers.get("Content-Range") != (
                                    f"bytes {start}-{end}/{ranges.sizes[index]}"
                                ):
                                    raise RuntimeError("HTTP range response mismatch")
                                data = response.read(length + 1)
                            if len(data) != length:
                                raise RuntimeError("Incomplete HTTP range")
                            ranges.cache[key] = data
                            if len(ranges.cache) > 256:
                                ranges.cache.popitem(last=False)
                        ranges.cache.move_to_end(key)
                        ranges.touched.add(index)
                        data = ranges.cache[key][offset - start:offset - start + capacity]
                        C.memmove(buffer, data, len(data))
                        out_read[0] = len(data)
                        return 0
                except Exception as exc:
                    self.error = str(exc)
                    return 13

            def close(self, context):
                self.released += 1

        return RangeSource()


class _Win10Ranges(_HttpTibxRanges):
    def __init__(self, peare_lib):
        names = ["Win10-2020-11-28.tibx"] + [f"Win10-2020-11-28-{i:04}.tibx" for i in range(1, 7)]
        super().__init__(peare_lib, "win-10-2020-11-28", names, [734003200] * 6 + [492163072])


@pytest.mark.skipif(os.environ.get("PEARE_TIBX_HTTP_RANGE") != "1",
                    reason="Set PEARE_TIBX_HTTP_RANGE=1 to read the public Win10 TIBX ranges")
def test_tibx_http_range_parts(peare_lib):
    expected = {
        14577303552: "a383e68c206d212d07e2ee2284a26e878b118db04cc7a430689f18dee7119600",
        838860800: "8c81cfe9ac42dd234ab2ab7bad3de61e7acdc3332a235d7c54166dcff2d1f754",
        104857600: "91d42c418f31cb92c92095662f06fe1eb19391289690fdd39aada0998edc771f",
    }
    ranges = _Win10Ranges(peare_lib)

    sources, opener = [], None
    try:
        for i in range(len(ranges.names)):
            sources.append(ranges.source(i))
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0 and opener is not None, [source.error for source in sources]
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
            source.handle = _HANDLE()
            assert source.released == 0
        found = {}

        def visit(current, depth):
            assert depth <= 3
            for folder in range(current.folder_count()):
                for index in range(current.resource_count(folder)):
                    resource = _HANDLE()
                    assert current._L.peare_opener_open_resource_at(current._h, folder, index, C.byref(resource)) == 0
                    try:
                        context = _Context()
                        assert current._L.peare_resource_get_context(resource, C.byref(context)) == 0
                        size, container = int(context.data_size), bool(context.is_container)
                        current._L.peare_resource_context_free(C.byref(context))
                        if size in expected:
                            found[size] = hashlib.sha256(peare_lib.read_resource(resource, 0, 4096)).hexdigest()
                        elif container:
                            child_source = _HANDLE()
                            assert current._L.peare_resource_get_source(resource, C.byref(child_source)) == 0
                            try:
                                child = peare_lib._open_source(child_source)
                                assert child is not None
                                with child:
                                    visit(child, depth + 1)
                            finally:
                                current._L.peare_source_destroy(child_source)
                    finally:
                        current._L.peare_resource_destroy(resource)

        visit(opener, 0)
        assert found == expected
        assert ranges.transferred <= ranges.limit
        assert ranges.touched == set(range(len(ranges.names)))
        print("TIBX HTTP range bytes:", ranges.transferred)
    finally:
        if opener is not None:
            opener.close()
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


def _find_tibx_volume(peare_lib, current, target_size, other_volume_sizes, depth=0):
    assert depth <= 3
    for folder in range(current.folder_count()):
        for index in range(current.resource_count(folder)):
            item = _HANDLE()
            assert current._L.peare_opener_open_resource_at(current._h, folder, index, C.byref(item)) == 0
            try:
                context = _Context()
                assert current._L.peare_resource_get_context(item, C.byref(context)) == 0
                size, container = int(context.data_size), bool(context.is_container)
                current._L.peare_resource_context_free(C.byref(context))
                if size == target_size:
                    result, item = item, _HANDLE()
                    return result
                if container and size not in other_volume_sizes:
                    nested = _HANDLE()
                    assert current._L.peare_resource_get_source(item, C.byref(nested)) == 0
                    try:
                        child = peare_lib._open_source(nested)
                        assert child is not None
                        with child:
                            result = _find_tibx_volume(peare_lib, child, target_size, other_volume_sizes, depth + 1)
                            if result:
                                return result
                    finally:
                        current._L.peare_source_destroy(nested)
            finally:
                if item:
                    current._L.peare_resource_destroy(item)
    return _HANDLE()


def _find_win10_volume(peare_lib, current):
    return _find_tibx_volume(peare_lib, current, 14577303552, (838860800, 104857600))


@pytest.mark.skipif(os.environ.get("PEARE_TIBX_HP_HTTP_RANGE") != "1",
                    reason="Set PEARE_TIBX_HP_HTTP_RANGE=1 to read the public HP TIBX ranges")
def test_tibx_hp_http_range_parts(peare_lib):
    base = "HP_ProBook_4520s_Win7_HB_x86_TR"
    names = [base + ".tibx"] + [f"{base}-{i:04}.tibx" for i in range(1, 5)]
    ranges = _HttpTibxRanges(peare_lib, "hp-pro-book-4520s-win-7-hb-x-86-tr", names,
                             [4660035584] * 4 + [3090501632])
    sources, opener, volume = [], None, _HANDLE()
    try:
        for i in range(len(names)):
            sources.append(ranges.source(i))
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0 and opener is not None, [source.error for source in sources]
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
            source.handle = _HANDLE()
            assert source.released == 0
        volume = _find_tibx_volume(peare_lib, opener, 302093697024,
                                   (13401849856, 314572800, 2147483648))
        assert volume, [source.error for source in sources]
        opener.close()
        opener = None
        assert all(source.released == 0 for source in sources)
        data = peare_lib.read_resource(volume, 0, 4096)
        assert len(data) == 4096
        assert hashlib.sha256(data).hexdigest() == (
            "7abc7f0335e81eea0d7a12b4b73cce47159f6c6a5cd108e1436518a5f1b3e77a"
        )
        assert ranges.touched == set(range(len(names)))
        assert ranges.transferred <= ranges.limit
    finally:
        if volume:
            peare_lib._lib.peare_resource_destroy(volume)
        if opener is not None:
            opener.close()
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
        print("HP TIBX HTTP range bytes:", ranges.transferred)
        print("HP TIBX source releases:", [source.released for source in sources])
    assert all(source.released == 1 for source in sources)


@pytest.mark.skipif(os.environ.get("PEARE_TIBX_HTTP_RANGE") != "1",
                    reason="Set PEARE_TIBX_HTTP_RANGE=1 to read the public Win10 TIBX ranges")
def test_wof_file_provider_http_range_atomic_failure(peare_lib):
    ranges = _Win10Ranges(peare_lib)
    sources, opened = [], []
    volume, resource = _HANDLE(), _HANDLE()
    volume_source = None

    class VolumeSource:
        def __init__(self, resource_handle):
            self.resource = resource_handle
            self.handle = _HANDLE()
            self.released = 0
            self.fail_at = None
            self.failures = 0
            self.reader = READ(self.read)
            self.release = RELEASE(self.close)
            create = peare_lib._lib.peare_source_create_callback
            create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, READ, RELEASE, C.POINTER(_HANDLE)]
            create.restype = C.c_int
            assert create(14577303552, b"volume.ntfs", None, self.reader, self.release,
                          C.byref(self.handle)) == 0

        def read(self, context, offset, buffer, capacity, out_read):
            out_read[0] = 0
            if self.fail_at is not None and offset <= self.fail_at < offset + capacity:
                if offset == self.fail_at:
                    self.failures += 1
                    return 13
                capacity = self.fail_at - offset
            return peare_lib._lib.peare_resource_read(self.resource, offset, buffer, capacity, out_read)

        def close(self, context):
            self.released += 1
            peare_lib._lib.peare_resource_destroy(self.resource)
            self.resource = _HANDLE()

    try:
        for i in range(len(ranges.names)):
            sources.append(ranges.source(i))
        status, root = _open_parts(peare_lib, sources)
        assert status == 0 and root is not None, [source.error for source in sources]
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
            source.handle = _HANDLE()
        try:
            volume = _find_win10_volume(peare_lib, root)
            assert volume, [source.error for source in sources]
        finally:
            root.close()
        assert all(source.released == 0 for source in sources)

        compressed_offset = 7181180928
        table = peare_lib.read_resource(volume, compressed_offset, 4)
        assert len(table) == 4
        second_chunk = compressed_offset + 4 + struct.unpack("<I", table)[0]
        assert compressed_offset + 4 < second_chunk < compressed_offset + 2261
        volume_source = VolumeSource(volume)
        volume = _HANDLE()
        status, current = _open_source_with_password(peare_lib, volume_source.handle, None)
        assert status == 0 and current is not None, [source.error for source in sources]
        opened.append(current)
        peare_lib._lib.peare_source_destroy(volume_source.handle)
        volume_source.handle = _HANDLE()
        assert volume_source.released == 0

        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        for name in ("Windows", "System32", "drivers"):
            directory = _HANDLE()
            assert find(current._h, b"NTFS_DIR", name.encode(), None, C.byref(directory)) == 0
            nested = _HANDLE()
            try:
                assert current._L.peare_resource_get_source(directory, C.byref(nested)) == 0
                status, current = _open_source_with_password(peare_lib, nested, None)
                assert status == 0 and current is not None, [source.error for source in sources]
                opened.append(current)
            finally:
                if nested:
                    peare_lib._lib.peare_source_destroy(nested)
                peare_lib._lib.peare_resource_destroy(directory)
        assert find(current._h, b"NTFS_FILE", b"null.sys", None, C.byref(resource)) == 0
        for opener in reversed(opened):
            opener.close()
        opened.clear()
        assert volume_source.released == 0 and all(source.released == 0 for source in sources)

        data = peare_lib.read_resource(resource, 0, 5120)
        assert len(data) == 5120
        assert hashlib.sha256(data).hexdigest() == (
            "26320a842fe8f6f0a2f8d17e225654a63d9ffa6df28a143628d9a08f320cdc62"
        )
        assert peare_lib.read_resource(resource, 0, 1) == data[:1]
        volume_source.fail_at = second_chunk
        output = (C.c_uint8 * 2)(0xa5, 0xa5)
        count = C.c_size_t(99)
        status = peare_lib._lib.peare_resource_read(resource, 4095, output, 2, C.byref(count))
        assert volume_source.failures > 0
        assert status == 13 and count.value == 0 and bytes(output) == b"\xa5\xa5"
        assert ranges.transferred <= ranges.limit
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        for opener in reversed(opened):
            opener.close()
        if volume_source is not None and volume_source.handle:
            peare_lib._lib.peare_source_destroy(volume_source.handle)
        if volume:
            peare_lib._lib.peare_resource_destroy(volume)
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
        print("TIBX WOF HTTP range bytes:", ranges.transferred)
        print("TIBX WOF source releases:",
              volume_source.released if volume_source is not None else None,
              [source.released for source in sources])
    assert volume_source is not None and volume_source.released == 1
    assert all(source.released == 1 for source in sources)


@pytest.mark.skipif(os.environ.get("PEARE_TIBX_HTTP_RANGE") != "1",
                    reason="Set PEARE_TIBX_HTTP_RANGE=1 to read the public Win10 TIBX ranges")
def test_wof_wim_http_range_real_payload(peare_lib):
    ranges = _Win10Ranges(peare_lib)
    sources, opened = [], []
    volume = directory = nested = volume_source = resource = _HANDLE()
    try:
        sources = [ranges.source(i) for i in range(len(ranges.names))]
        status, root = _open_parts(peare_lib, sources)
        assert status == 0 and root is not None, [source.error for source in sources]
        opened.append(root)
        volume = _find_win10_volume(peare_lib, root)
        assert volume, [source.error for source in sources]
        volume_source = _HANDLE()
        assert root._L.peare_resource_get_source(volume, C.byref(volume_source)) == 0
        status, ntfs = _open_source_with_password(peare_lib, volume_source, None)
        assert status == 0 and ntfs is not None
        opened.append(ntfs)
        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        directory = _HANDLE()
        assert find(ntfs._h, b"NTFS_DIR", b"Windows", None, C.byref(directory)) == 0
        nested = _HANDLE()
        assert ntfs._L.peare_resource_get_source(directory, C.byref(nested)) == 0
        status, windows = _open_source_with_password(peare_lib, nested, None)
        assert status == 0 and windows is not None
        opened.append(windows)
        resource = _HANDLE()
        assert windows._L.peare_opener_open_resource_at(windows._h, 0, 16, C.byref(resource)) == 0
        data = peare_lib.read_resource(resource, 0, 70434)
        assert len(data) == 70434
        assert hashlib.sha256(data).hexdigest() == (
            "7a1d7bb6489e79b428592ff3d22f9890c6dbc4e08f4ef3406a37b0bbe27297c8"
        )
        assert ranges.transferred <= ranges.limit
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if nested:
            peare_lib._lib.peare_source_destroy(nested)
        if directory:
            peare_lib._lib.peare_resource_destroy(directory)
        if volume_source:
            peare_lib._lib.peare_source_destroy(volume_source)
        if volume:
            peare_lib._lib.peare_resource_destroy(volume)
        for opener in reversed(opened):
            opener.close()
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
    assert all(source.released == 1 for source in sources)


@pytest.mark.skipif(os.environ.get("PEARE_TIBX_HTTP_RANGE") != "1",
                    reason="Set PEARE_TIBX_HTTP_RANGE=1 to read the public Win10 TIBX ranges")
def test_wof_wim_http_range_mutation_and_cache(peare_lib):
    ranges = _Win10Ranges(peare_lib)
    sources, opened = [], []
    volume, resource = _HANDLE(), _HANDLE()
    volume_source = None

    class MutableVolumeSource:
        def __init__(self, resource_handle):
            self.resource = resource_handle
            self.handle = _HANDLE()
            self.released = 0
            self.mutate = False
            self.mutations = 0
            self.reader = READ(self.read)
            self.release = RELEASE(self.close)
            create = peare_lib._lib.peare_source_create_callback
            create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, READ, RELEASE, C.POINTER(_HANDLE)]
            create.restype = C.c_int
            assert create(14577303552, b"volume.ntfs", None, self.reader, self.release,
                          C.byref(self.handle)) == 0

        def read(self, context, offset, buffer, capacity, out_read):
            out_read[0] = 0
            status = peare_lib._lib.peare_resource_read(self.resource, offset, buffer, capacity, out_read)
            if status == 0 and self.mutate and out_read[0]:
                buffer[0] ^= 1
                self.mutations += 1
            return status

        def close(self, context):
            self.released += 1
            peare_lib._lib.peare_resource_destroy(self.resource)
            self.resource = _HANDLE()

    try:
        for i in range(len(ranges.names)):
            sources.append(ranges.source(i))
        status, root = _open_parts(peare_lib, sources)
        assert status == 0 and root is not None, [source.error for source in sources]
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)
            source.handle = _HANDLE()
        try:
            volume = _find_win10_volume(peare_lib, root)
            assert volume, [source.error for source in sources]
        finally:
            root.close()
        assert all(source.released == 0 for source in sources)

        volume_source = MutableVolumeSource(volume)
        volume = _HANDLE()
        status, ntfs = _open_source_with_password(peare_lib, volume_source.handle, None)
        assert status == 0 and ntfs is not None, [source.error for source in sources]
        opened.append(ntfs)
        peare_lib._lib.peare_source_destroy(volume_source.handle)
        volume_source.handle = _HANDLE()
        assert volume_source.released == 0

        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        directory, nested = _HANDLE(), _HANDLE()
        try:
            assert find(ntfs._h, b"NTFS_DIR", b"Windows", None, C.byref(directory)) == 0
            assert ntfs._L.peare_resource_get_source(directory, C.byref(nested)) == 0
            status, windows = _open_source_with_password(peare_lib, nested, None)
            assert status == 0 and windows is not None, [source.error for source in sources]
            opened.append(windows)
        finally:
            if nested:
                peare_lib._lib.peare_source_destroy(nested)
            if directory:
                peare_lib._lib.peare_resource_destroy(directory)

        assert windows._L.peare_opener_open_resource_at(windows._h, 0, 16, C.byref(resource)) == 0
        context = _Context()
        assert windows._L.peare_resource_get_context(resource, C.byref(context)) == 0
        try:
            assert int(context.data_size) == 70434
        finally:
            windows._L.peare_resource_context_free(C.byref(context))
        for opener in reversed(opened):
            opener.close()
        opened.clear()
        assert volume_source.released == 0 and all(source.released == 0 for source in sources)

        size = 70434
        original = peare_lib.read_resource(resource, 0, size)
        assert len(original) == size
        assert hashlib.sha256(original).hexdigest() == (
            "7a1d7bb6489e79b428592ff3d22f9890c6dbc4e08f4ef3406a37b0bbe27297c8"
        )
        assert peare_lib.read_resource(resource, 0, 1) == original[:1]
        volume_source.mutate = True
        cached = peare_lib.read_resource(resource, 0, 127)
        assert cached == original[:127] and volume_source.mutations == 0
        for offset, length in ((65528, 16), (65536, 127), (1, 127), (65537, 127), (0, 127)):
            output = (C.c_uint8 * length)(*([0xa5] * length))
            count = C.c_size_t(99)
            status = peare_lib._lib.peare_resource_read(resource, offset, output, length, C.byref(count))
            if status == 0:
                assert count.value == length and bytes(output) == original[offset:offset + length]
            else:
                assert count.value == 0 and bytes(output) == b"\xa5" * length
        assert volume_source.mutations > 0
        assert ranges.transferred <= ranges.limit
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        for opener in reversed(opened):
            opener.close()
        if volume_source is not None and volume_source.handle:
            peare_lib._lib.peare_source_destroy(volume_source.handle)
        if volume:
            peare_lib._lib.peare_resource_destroy(volume)
        for source in sources:
            if source.handle:
                peare_lib._lib.peare_source_destroy(source.handle)
        print("TIBX WOF WIM HTTP range bytes:", ranges.transferred)
        print("TIBX WOF WIM source releases:",
              volume_source.released if volume_source is not None else None,
              [source.released for source in sources])
    assert volume_source is not None and volume_source.released == 1
    assert all(source.released == 1 for source in sources)


def test_callback_source_tracked_tar(peare_lib):
    source = CallbackSource(peare_lib, TAR_SAMPLE)
    status, opener = _open_source_with_password(peare_lib, source.handle, None)
    assert status == 0 and opener is not None
    peare_lib._lib.peare_source_destroy(source.handle)
    resource, size = _find_resource(opener, "README.md")
    opener.close()
    try:
        assert source.released == 0
        assert size == 30892
        assert hashlib.sha256(peare_lib.read_resource(resource, 0, size)).hexdigest() == (
            "2709e7c6a379270a75fab71ccad78186e3487d9f3ae1afbca8890aaed080f54d"
        )
    finally:
        peare_lib._lib.peare_resource_destroy(resource)
    assert source.released == 1


@requires_samples
@pytest.mark.parametrize(("sample", "directories", "name", "size", "digest"), [
    ("testAcronis_full_b1_s1_v1.tib", ("C:", "testAcronis"),
     "100MB.bin:Zone.Identifier:$DATA", 109,
     "f870e553a2c6289e1ee41c5d06eb49c0ba2efcdeeee0dccfc37dc589da561bd3"),
    ("t1_legacy_tib.tibx", ("H:", "docs"), "doc_00.txt", 2048,
     "c9c981f0ae0f298bdfdf638a4e83361bce220eff307b6db95713fff27b7999ba"),
])
def test_callback_source_nested_lifetime_and_reads(peare_lib, sample, directories, name, size, digest):
    source = CallbackSource(peare_lib, Path(SAMPLES) / sample)
    status, current = _open_source_with_password(peare_lib, source.handle, None)
    assert status == 0 and current is not None
    peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 0
    for directory in directories:
        nested = _directory_source(current, directory)
        current.close()
        assert source.released == 0
        try:
            status, current = _open_source_with_password(peare_lib, nested, None)
            assert status == 0 and current is not None
        finally:
            peare_lib._lib.peare_source_destroy(nested)
    resource, actual_size = _find_resource(current, name)
    current.close()
    try:
        assert source.released == 0
        assert actual_size == size
        data = peare_lib.read_resource(resource, 0, size)
        assert len(data) == size and hashlib.sha256(data).hexdigest() == digest
        for offset in (0, size // 2, size - 1):
            count = min(8, size - offset)
            assert peare_lib.read_resource(resource, offset, count) == data[offset:offset + count]
        def positioned(worker):
            for i in range(50):
                offset = (worker * 73 + i * 31) % size
                count = min(17, size - offset)
                assert peare_lib.read_resource(resource, offset, count) == data[offset:offset + count]
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(positioned, range(4)))
        assert source.calls > 1
        assert all(offset < source.size and capacity <= source.size - offset
                   for offset, capacity in source.ranges)
    finally:
        peare_lib._lib.peare_resource_destroy(resource)
    assert source.released == 1


@pytest.mark.parametrize("mode", ["error", "zero", "overcount"])
def test_callback_source_errors_do_not_open(peare_lib, mode):
    source = CallbackSource(peare_lib, TAR_SAMPLE, mode)
    status, opener = _open_source_with_password(peare_lib, source.handle, None)
    assert status == 13 and opener is None
    assert source.calls > 0
    assert source.released == 0
    peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


def test_callback_source_partial_header_then_error(peare_lib):
    source = CallbackSource(peare_lib, TAR_SAMPLE, "partial_then_error")
    try:
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        if opener is not None:
            opener.close()
        assert status != 0 and opener is None
        assert source.calls >= 2
    finally:
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


def test_xfs_extent_read_propagates_source_failure(peare_lib):
    image = os.environ.get("PEARE_XFS_IMAGE")
    if not image:
        pytest.skip("Set PEARE_XFS_IMAGE to the independent XFS VHDX sample")
    source = CallbackSource(peare_lib, Path(image))
    opened = []
    resource = _HANDLE()
    try:
        status, root = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and root is not None
        opened.append(root)
        rows = root.normalized_tree(max_depth=0, recurse_containers=False)
        assert len(rows) == 1 and rows[0]["format"] == "XFS"
        partition = _directory_source(root, rows[0]["path"])
        try:
            status, xfs = _open_source_with_password(peare_lib, partition, None)
        finally:
            peare_lib._lib.peare_source_destroy(partition)
        assert status == 0 and xfs is not None
        opened.append(xfs)
        folder = _directory_source(xfs, "folder")
        try:
            status, nested = _open_source_with_password(peare_lib, folder, None)
        finally:
            peare_lib._lib.peare_source_destroy(folder)
        assert status == 0 and nested is not None
        opened.append(nested)
        reference, length = _find_resource(nested, "file.100")
        try:
            assert hashlib.md5(peare_lib.read_resource(reference, 0, length)).hexdigest() == (
                "620f0b67a91f7f74151bc5be745b7110"
            )
        finally:
            peare_lib._lib.peare_resource_destroy(reference)
        resource, length = _find_resource(xfs, "data")
        source.mode = "error"
        calls = source.calls
        for offset in range(0, length, 1024 * 1024):
            output = (C.c_uint8 * 32)()
            count = C.c_size_t(99)
            status = peare_lib._lib.peare_resource_read(resource, offset, output, 32, C.byref(count))
            if source.calls > calls:
                assert status != 0 and count.value == 0
                break
        else:
            pytest.fail("The XFS extent did not request data from the failing source")
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        for opener in reversed(opened):
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@requires_samples
def test_callback_source_error_during_lazy_read(peare_lib):
    source = CallbackSource(peare_lib, Path(SAMPLES) / "t1_stored.tibx")
    status, root = _open_source_with_password(peare_lib, source.handle, None)
    assert status == 0 and root is not None
    nested = _directory_source(root, "disk1")
    status, disk = _open_source_with_password(peare_lib, nested, None)
    assert status == 0 and disk is not None
    peare_lib._lib.peare_source_destroy(nested)
    resource, size = _find_resource(disk, "part1")
    source.mode = "error"
    buffer = (C.c_uint8 * 32)()
    before = source.calls
    for offset in range(0, size, 1024 * 1024):
        count = C.c_size_t(99)
        status = disk._L.peare_resource_read(resource, offset, buffer, len(buffer), C.byref(count))
        if source.calls > before:
            break
        assert status == 0
    assert source.calls > before
    assert status == 13 and count.value == 0
    peare_lib._lib.peare_resource_destroy(resource)
    disk.close()
    root.close()
    peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@requires_samples
def test_real_ntfs_callback_mft_read_failure_and_retry(peare_lib):
    root = peare_lib.open_file(str(Path(SAMPLES) / "t1_chain_inc.tibx"))
    assert root is not None
    opened = [root]
    volume = None
    callback_sources = []
    try:
        current = root
        for name in ("3", "disk1"):
            source = _directory_source(current, name)
            try:
                status, current = _open_source_with_password(peare_lib, source, None)
            finally:
                peare_lib._lib.peare_source_destroy(source)
            assert status == 0 and current is not None
            opened.append(current)
        volume, size = _find_resource(current, "part1")
        bpb = peare_lib.read_resource(volume, 0, 512)
        assert bpb[3:11] == b"NTFS    "
        sector = struct.unpack_from("<H", bpb, 11)[0]
        cluster = sector * bpb[13]
        mft = struct.unpack_from("<Q", bpb, 48)[0] * cluster
        assert 0 < mft < size
        failures = [0]

        def make_source(fail):
            def read(context, offset, buffer, capacity, out_read):
                if fail and offset <= mft < offset + capacity:
                    failures[0] += 1
                    out_read[0] = 0
                    return 13
                return peare_lib._lib.peare_resource_read(
                    volume, offset, buffer, capacity, out_read)
            callback = READ(read)
            handle = _HANDLE()
            create = peare_lib._lib.peare_source_create_callback
            create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, READ, RELEASE, C.POINTER(_HANDLE)]
            create.restype = C.c_int
            assert create(size, b"volume.ntfs", None, callback, RELEASE(), C.byref(handle)) == 0
            callback_sources.append((handle, callback))
            return handle

        status, failed = _open_source_with_password(peare_lib, make_source(True), None)
        if failed is not None:
            failed.close()
        assert failures[0] > 0 and status != 0 and failed is None
        status, healthy = _open_source_with_password(peare_lib, make_source(False), None)
        assert status == 0 and healthy is not None
        try:
            assert healthy.container_format() == "NTFS"
        finally:
            healthy.close()
    finally:
        for handle, callback in callback_sources:
            peare_lib._lib.peare_source_destroy(handle)
        if volume:
            peare_lib._lib.peare_resource_destroy(volume)
        for opener in reversed(opened):
            opener.close()


def test_real_wof_file_provider_chunk_failure_is_atomic(peare_lib):
    image = os.environ.get("PEARE_WOF_FILE_NTFS_IMAGE")
    if not image:
        pytest.skip("Set PEARE_WOF_FILE_NTFS_IMAGE to the real Win10 TIBX volume 11")
    path = Path(image)
    assert path.stat().st_size == 14577303552
    compressed_offset = 7181180928
    with path.open("rb") as volume:
        volume.seek(compressed_offset)
        table = volume.read(4)
    assert len(table) == 4
    second_chunk = compressed_offset + 4 + struct.unpack("<I", table)[0]
    assert compressed_offset + 4 < second_chunk < compressed_offset + 2261

    class FailSecondChunk(CallbackSource):
        def __init__(self, lib, image_path):
            self.fail_at = None
            self.failures = 0
            super().__init__(lib, image_path)

        def read(self, context, offset, buffer, capacity, out_read):
            if self.fail_at is not None and offset <= self.fail_at < offset + min(capacity, 127):
                self.failures += 1
                out_read[0] = 0
                return 13
            return super().read(context, offset, buffer, capacity, out_read)

    source = FailSecondChunk(peare_lib, path)
    opened, resource = [], _HANDLE()
    try:
        status, current = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and current is not None
        opened.append(current)
        for name in ("Windows", "System32", "drivers"):
            nested = _directory_source(current, name)
            try:
                status, current = _open_source_with_password(peare_lib, nested, None)
            finally:
                peare_lib._lib.peare_source_destroy(nested)
            assert status == 0 and current is not None
            opened.append(current)
        resource, size = _find_resource(current, "null.sys")
        assert size == 5120
        data = peare_lib.read_resource(resource, 0, size)
        assert hashlib.sha256(data).hexdigest() == (
            "26320a842fe8f6f0a2f8d17e225654a63d9ffa6df28a143628d9a08f320cdc62"
        )
        assert peare_lib.read_resource(resource, 0, 1) == data[:1]
        source.fail_at = second_chunk
        output = (C.c_uint8 * 2)(0xa5, 0xa5)
        count = C.c_size_t(99)
        status = peare_lib._lib.peare_resource_read(resource, 4095, output, 2, C.byref(count))
        assert source.failures > 0
        assert status == 13 and count.value == 0
        assert bytes(output) == b"\xa5\xa5"
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        for opener in reversed(opened):
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@pytest.mark.parametrize("fail_read", [False, True])
def test_real_wof_wim_backing_lookup_read_failure(peare_lib, fail_read):
    volume_path = os.environ.get("PEARE_WOF_WIM_NTFS_IMAGE")
    item_path = os.environ.get("PEARE_WOF_WIM_ITEM_PATH")
    if not volume_path or not item_path:
        pytest.skip("Set PEARE_WOF_WIM_NTFS_IMAGE and PEARE_WOF_WIM_ITEM_PATH for the real WOF sample")
    source = CallbackSource(peare_lib, Path(volume_path))
    opened = []
    resource = _HANDLE()
    try:
        status, current = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and current is not None
        opened.append(current)
        parts = item_path.replace("\\", "/").strip("/").split("/")
        for name in parts[:-1]:
            nested = _directory_source(current, name)
            try:
                status, current = _open_source_with_password(peare_lib, nested, None)
            finally:
                peare_lib._lib.peare_source_destroy(nested)
            assert status == 0 and current is not None
            opened.append(current)
        source.mode = "error" if fail_read else None
        calls = source.calls
        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        status = find(current._h, b"NTFS_FILE", parts[-1].encode("utf-8"), None, C.byref(resource))
        if fail_read:
            assert source.calls > calls
            assert status != 0 and not resource
        else:
            assert status == 0 and resource
            data = peare_lib.read_resource(resource, 0, 70434)
            assert len(data) == 70434
            assert hashlib.sha256(data).hexdigest() == "7a1d7bb6489e79b428592ff3d22f9890c6dbc4e08f4ef3406a37b0bbe27297c8"
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        for opener in reversed(opened):
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)


def test_real_wof_wim_mutable_source_and_positioned_reads(peare_lib):
    volume_path = os.environ.get("PEARE_WOF_WIM_NTFS_IMAGE")
    item_path = os.environ.get("PEARE_WOF_WIM_ITEM_PATH")
    if not volume_path or not item_path:
        pytest.skip("Set PEARE_WOF_WIM_NTFS_IMAGE and PEARE_WOF_WIM_ITEM_PATH for the real WOF sample")

    class MutableCallback(CallbackSource):
        def __init__(self, lib, path):
            self.mutate = False
            self.mutations = 0
            super().__init__(lib, path)

        def read(self, context, offset, buffer, capacity, out_read):
            status = super().read(context, offset, buffer, capacity, out_read)
            if status == 0 and self.mutate and out_read[0]:
                buffer[0] ^= 1
                self.mutations += 1
            return status

    source = MutableCallback(peare_lib, Path(volume_path))
    opened = []
    resource = _HANDLE()
    try:
        status, current = _open_source_with_password(peare_lib, source.handle, None)
        assert status == 0 and current is not None
        opened.append(current)
        parts = item_path.replace("\\", "/").strip("/").split("/")
        for name in parts[:-1]:
            nested = _directory_source(current, name)
            try:
                status, current = _open_source_with_password(peare_lib, nested, None)
            finally:
                peare_lib._lib.peare_source_destroy(nested)
            assert status == 0 and current is not None
            opened.append(current)
        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        assert find(current._h, b"NTFS_FILE", parts[-1].encode("utf-8"), None, C.byref(resource)) == 0
        size = 70434
        reference = peare_lib.read_resource(resource, 0, size)
        assert len(reference) == size
        assert hashlib.sha256(reference).hexdigest() == "7a1d7bb6489e79b428592ff3d22f9890c6dbc4e08f4ef3406a37b0bbe27297c8"

        def reads(worker):
            for i in range(40):
                offset = (worker * 16673 + i * 997) % size
                count = min(257, size - offset)
                assert peare_lib.read_resource(resource, offset, count) == reference[offset:offset + count]
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(reads, range(4)))

        assert peare_lib.read_resource(resource, 65528, 8) == reference[65528:65536]
        source.mutate = True
        for offset, length in ((65528, 16), (0, 127), (65536, 127), (1, 127), (65537, 127)):
            buffer = (C.c_uint8 * length)(*([0xa5] * length))
            read = C.c_size_t()
            status = peare_lib._lib.peare_resource_read(resource, offset, buffer, len(buffer), C.byref(read))
            if status == 0:
                assert read.value == len(buffer)
                assert bytes(buffer[:read.value]) == reference[offset:offset + read.value]
            else:
                assert read.value == 0
                assert bytes(buffer) == b"\xa5" * length
            if source.mutations:
                break
        assert source.mutations > 0, "The mutation did not reach the WOF backing source"
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        for opener in reversed(opened):
            opener.close()
        peare_lib._lib.peare_source_destroy(source.handle)


@requires_samples
def test_tibx_callback_current_header_io_failure_does_not_fall_back(peare_lib):
    class FailCurrentHeader(CallbackSource):
        def __init__(self, lib, path):
            self.failures = 0
            super().__init__(lib, path)

        def read(self, context, offset, buffer, capacity, out_read):
            if offset < 999424 and offset + capacity > 995328:
                self.calls += 1
                self.failures += 1
                return 13
            return super().read(context, offset, buffer, capacity, out_read)

    source = FailCurrentHeader(peare_lib, Path(SAMPLES) / "t1_chain_inc.tibx")
    try:
        status, opener = _open_source_with_password(peare_lib, source.handle, None)
        if opener is not None:
            opener.close()
        assert status != 0 and opener is None
        assert source.failures > 0
    finally:
        peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


def test_callback_source_rejects_unrepresentable_length_without_ownership(peare_lib):
    source = CallbackSource(peare_lib, TAR_SAMPLE)
    rejected = _HANDLE()
    status = peare_lib._lib.peare_source_create_callback(
        1 << 63, b"archive.tibx", None, source.reader, source.release, C.byref(rejected))
    assert status == 5 and not rejected
    assert source.released == 0
    peare_lib._lib.peare_source_destroy(source.handle)
    assert source.released == 1


@requires_samples
def test_tibx_ntfs_directory_index_read_failure(peare_lib):
    root = peare_lib.open_file(str(Path(SAMPLES) / "t1_chain_inc.tibx"))
    owners = [root]
    volume = None
    callback_source = _HANDLE()
    ntfs = None
    child = None
    directory_source = _HANDLE()
    armed = [False]
    failures = [0]
    releases = []
    try:
        current = root
        for name in ("3", "disk1"):
            source = _directory_source(current, name)
            try:
                status, current = _open_source_with_password(peare_lib, source, None)
                assert status == 0 and current is not None
                owners.append(current)
            finally:
                peare_lib._lib.peare_source_destroy(source)
        volume, volume_size = _find_resource(current, "part1")

        @READ
        def read_volume(context, offset, buffer, capacity, out_read):
            status = peare_lib._lib.peare_resource_read(volume, offset, buffer, capacity, out_read)
            if status == 0 and armed[0] and out_read[0] >= 4 and C.string_at(buffer, 4) == b"INDX":
                failures[0] += 1
                return 13
            return status

        @RELEASE
        def release_volume(context):
            releases.append(True)

        create = peare_lib._lib.peare_source_create_callback
        create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, READ, RELEASE, C.POINTER(_HANDLE)]
        create.restype = C.c_int
        assert create(volume_size, b"partition.img", None, read_volume, release_volume,
                      C.byref(callback_source)) == 0
        status, ntfs = _open_source_with_password(peare_lib, callback_source, None)
        assert status == 0 and ntfs is not None
        directory_source = _directory_source(ntfs, "System Volume Information")
        armed[0] = True
        status, child = _open_source_with_password(peare_lib, directory_source, None)
        assert failures[0] > 0
        assert status != 0 and child is None
    finally:
        if child:
            child.close()
        if directory_source:
            peare_lib._lib.peare_source_destroy(directory_source)
        if ntfs:
            ntfs.close()
        if callback_source:
            peare_lib._lib.peare_source_destroy(callback_source)
        if volume:
            peare_lib._lib.peare_resource_destroy(volume)
        for opener in reversed(owners):
            opener.close()
    assert releases == [True]


@requires_samples
@pytest.mark.parametrize("fail_open", [False, True])
@pytest.mark.parametrize(("directory", "filename", "expected_size", "digest"), [
    ("System Volume Information", "MasterFileStatus.db", 22464,
     "9e36ef9f46ead13e7359a7e56cc6e9bc64b90bccba9c46906e0be40b465dee2e"),
    ("ntfs", "lznt1_compressed.txt", 16384,
     "8f4c07a3841244b053f89d11c0a63d124c9cfc2811de36dea64f7ae6c4422d01"),
])
def test_callback_ntfs_enumeration_defers_file_open(peare_lib, directory, filename, expected_size, digest, fail_open):
    root = peare_lib.open_file(str(Path(SAMPLES) / "t1_chain_inc.tibx"))
    assert root is not None
    owners = [root]
    volume = None
    callback_source = _HANDLE()
    ntfs = None
    child = None
    file_resource = _HANDLE()
    releases = []
    reads = []
    failure = [False]
    try:
        current = root
        for name in ("3", "disk1"):
            source = _directory_source(current, name)
            try:
                status, current = _open_source_with_password(peare_lib, source, None)
                assert status == 0 and current is not None
                owners.append(current)
            finally:
                peare_lib._lib.peare_source_destroy(source)
        volume, volume_size = _find_resource(current, "part1")
        @READ
        def read_volume(context, offset, buffer, capacity, out_read):
            reads.append((offset, capacity))
            if failure[0]:
                return 13
            return peare_lib._lib.peare_resource_read(volume, offset, buffer, capacity, out_read)
        @RELEASE
        def release_volume(context):
            releases.append(True)
        create = peare_lib._lib.peare_source_create_callback
        create.argtypes = [C.c_uint64, C.c_char_p, C.c_void_p, READ, RELEASE, C.POINTER(_HANDLE)]
        create.restype = C.c_int
        assert create(volume_size, b"partition.img", None, read_volume, release_volume,
                      C.byref(callback_source)) == 0
        status, ntfs = _open_source_with_password(peare_lib, callback_source, None)
        assert status == 0 and ntfs is not None and ntfs.container_format() == "NTFS"
        find = peare_lib._lib.peare_opener_find_resource
        find.argtypes = [_HANDLE, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(_HANDLE)]
        find.restype = C.c_int
        directory_resource = _HANDLE()
        assert find(ntfs._h, b"NTFS_DIR", directory.encode(), None, C.byref(directory_resource)) == 0
        directory_source = _HANDLE()
        try:
            assert peare_lib._lib.peare_resource_get_source(directory_resource, C.byref(directory_source)) == 0
        finally:
            peare_lib._lib.peare_resource_destroy(directory_resource)
        try:
            status, child = _open_source_with_password(peare_lib, directory_source, None)
            assert status == 0 and child is not None
        finally:
            peare_lib._lib.peare_source_destroy(directory_source)
        before = len(reads)
        counts = [child.resource_count(i) for i in range(child.folder_count())]
        assert sum(counts) > 0 and len(reads) == before
        failure[0] = fail_open
        status = find(child._h, b"NTFS_FILE", filename.encode(), None, C.byref(file_resource))
        assert len(reads) > before
        if fail_open:
            assert status == 13 and not file_resource
        else:
            assert status == 0
            context = _Context()
            assert peare_lib._lib.peare_resource_get_context(file_resource, C.byref(context)) == 0
            try:
                assert int(context.data_size) == expected_size
            finally:
                peare_lib._lib.peare_resource_context_free(C.byref(context))
            data = peare_lib.read_resource(file_resource, 0, expected_size)
            assert len(data) == expected_size and hashlib.sha256(data).hexdigest() == digest
            def check_reads(worker):
                for i in range(30):
                    offset = (worker * 733 + i * 251) % expected_size
                    size = min(97, expected_size - offset)
                    assert peare_lib.read_resource(file_resource, offset, size) == data[offset:offset + size]
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(check_reads, range(4)))
    finally:
        if file_resource:
            peare_lib._lib.peare_resource_destroy(file_resource)
        if child:
            child.close()
        if ntfs:
            ntfs.close()
        if callback_source:
            peare_lib._lib.peare_source_destroy(callback_source)
        if volume:
            peare_lib._lib.peare_resource_destroy(volume)
        for opener in reversed(owners):
            opener.close()
    assert releases == [True]
