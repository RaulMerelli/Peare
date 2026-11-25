"""Check split WIM parts through the public Opener ABI."""

from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import ctypes as C
import hashlib
import shutil

import pytest

from test_callback_source_abi import CallbackSource, _find_resource, _open_parts
from peare_opener import _HANDLE


FIXTURES = Path(__file__).resolve().parent / "formats" / "wim" / "fixtures"
PAYLOADS = {
    "payload-1.bin": "654d2fba08fcb04660c46daa1323024545ebace16913533dd37b705726f6fcdc",
    "payload-2.bin": "69ef50559afeeb2b48695075ef95119373b49f3497caa26b0b34269e1177f1ba",
    "payload-3.bin": "e2a11780fc95d24db4867636e676048f69b8094b85db89a2d58e3b9d83aedfb4",
    "payload-4.bin": "a4ba7b8c701671de76fad7f13ca46560c1484e47c1c2e090b288d772e87848ac",
}


@pytest.mark.parametrize("part", ("test.swm", "test2.swm"))
def test_split_wim_parts_open_the_complete_image(peare_lib, part):
    opener = peare_lib.open_file(str(FIXTURES / part))
    assert opener is not None
    try:
        assert opener.container_format() == "WIM"
        rows = opener.normalized_tree()
        assert {row["path"] for row in rows} == set(PAYLOADS)
        assert all(row["size"] == 409600 for row in rows)
        assert {row["path"]: row["sha256"] for row in rows} == PAYLOADS
    finally:
        opener.close()


def test_split_wim_callback_sources_open_the_complete_image(peare_lib):
    sources = [CallbackSource(peare_lib, FIXTURES / part)
               for part in ("test.swm", "test2.swm")]
    try:
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0
        try:
            assert opener.container_format() == "WIM"
            assert {row["path"]: row["sha256"] for row in opener.normalized_tree()} == PAYLOADS
        finally:
            opener.close()
    finally:
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)


def test_split_wim_callback_sources_reject_reversed_parts(peare_lib):
    sources = [CallbackSource(peare_lib, FIXTURES / part)
               for part in ("test2.swm", "test.swm")]
    try:
        status, opener = _open_parts(peare_lib, sources)
        assert status != 0
        assert opener is None
    finally:
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)


def test_split_wim_numbered_base_from_later_part(peare_lib, tmp_path):
    shutil.copyfile(FIXTURES / "wim-small.swm", tmp_path / "image1.swm")
    shutil.copyfile(FIXTURES / "wim-small2.swm", tmp_path / "image12.swm")
    opener = peare_lib.open_file(str(tmp_path / "image12.swm"))
    assert opener is not None
    try:
        assert opener.container_format() == "WIM"
        assert {row["path"] for row in opener.normalized_tree()} == {
            "empty", "nested", "nested/README.md", "README.md"
        }
    finally:
        opener.close()


@pytest.mark.parametrize("via_parts", (False, True))
def test_split_wim_directories_keep_all_parts(peare_lib, via_parts):
    sources = []
    if via_parts:
        sources = [CallbackSource(peare_lib, FIXTURES / part)
                   for part in ("wim-small.swm", "wim-small2.swm")]
        status, opener = _open_parts(peare_lib, sources)
        assert status == 0
    else:
        opener = peare_lib.open_file(str(FIXTURES / "wim-small.swm"))
    assert opener is not None
    try:
        for directory in ("empty", "nested"):
            resource, _ = _find_resource(opener, directory)
            source = _HANDLE()
            try:
                assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
                child = peare_lib._open_source(source)
                assert child is not None
                try:
                    rows = child.normalized_tree()
                    if directory == "empty":
                        assert rows == []
                    else:
                        assert {row["path"]: row["sha256"] for row in rows} == {
                            "README.md": "db58e70bf344b11d839520e667f91292e19601a9fbfbc86ed8ce74090582e1fb"
                        }
                finally:
                    child.close()
            finally:
                peare_lib._lib.peare_source_destroy(source)
                peare_lib._lib.peare_resource_destroy(resource)
    finally:
        opener.close()
        for source in sources:
            peare_lib._lib.peare_source_destroy(source.handle)


def test_split_wim_directory_concurrent_opens(peare_lib):
    opener = peare_lib.open_file(str(FIXTURES / "wim-small.swm"))
    assert opener is not None
    resource, _ = _find_resource(opener, "nested")
    source = _HANDLE()
    try:
        assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0

        def open_directory(_):
            child = peare_lib._open_source(source)
            assert child is not None
            try:
                rows = child.normalized_tree()
                assert {row["path"]: row["sha256"] for row in rows} == {
                    "README.md": "db58e70bf344b11d839520e667f91292e19601a9fbfbc86ed8ce74090582e1fb"
                }
            finally:
                child.close()

        with ThreadPoolExecutor(max_workers=6) as pool:
            list(pool.map(open_directory, range(30)))
    finally:
        peare_lib._lib.peare_source_destroy(source)
        peare_lib._lib.peare_resource_destroy(resource)
        opener.close()


def test_split_wim_directory_survives_parent_close(peare_lib):
    opener = peare_lib.open_file(str(FIXTURES / "wim-small.swm"))
    assert opener is not None
    resource, _ = _find_resource(opener, "nested")
    source = _HANDLE()
    try:
        assert opener._L.peare_resource_get_source(resource, C.byref(source)) == 0
        opener._L.peare_resource_destroy(resource)
        resource = None
        opener.close()
        opener = None
        child = peare_lib._open_source(source)
        assert child is not None
        try:
            assert {row["path"] for row in child.normalized_tree()} == {"README.md"}
        finally:
            child.close()
    finally:
        peare_lib._lib.peare_source_destroy(source)
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        if opener:
            opener.close()


def test_split_wim_concurrent_positioned_reads(peare_lib):
    opener = peare_lib.open_file(str(FIXTURES / "test.swm"))
    assert opener is not None
    resource = None
    try:
        resource, size = _find_resource(opener, "payload-1.bin")
        expected = peare_lib.read_resource(resource, 0, size)
        assert hashlib.sha256(expected).hexdigest() == PAYLOADS["payload-1.bin"]

        def read_chunks(worker):
            for index in range(80):
                offset = (worker * 37001 + index * 19003) % size
                length = min(4096, size - offset)
                assert peare_lib.read_resource(resource, offset, length) == expected[offset:offset + length]

        with ThreadPoolExecutor(max_workers=6) as pool:
            list(pool.map(read_chunks, range(6)))
    finally:
        if resource:
            peare_lib._lib.peare_resource_destroy(resource)
        opener.close()
