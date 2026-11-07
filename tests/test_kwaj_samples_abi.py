"""Compare authentic KWAJ samples with independent decoded outputs via the C ABI."""

import hashlib
import os
import ctypes as C
from pathlib import Path

import pytest
import peare_opener as po


SAMPLE_ROOT = os.environ.get("PEARE_KWAJ_SAMPLE_ROOT")
ORACLE_ROOT = os.environ.get("PEARE_KWAJ_ORACLE_ROOT")
UPSTREAM_ROOT = os.environ.get("PEARE_KWAJ_UPSTREAM_SAMPLE_ROOT")
UPSTREAM_ORACLE_ROOT = os.environ.get("PEARE_KWAJ_UPSTREAM_ORACLE_ROOT")
MSZIP_SAMPLE = os.environ.get("PEARE_KWAJ_MSZIP_SAMPLE")
MSZIP_ORACLE = os.environ.get("PEARE_KWAJ_MSZIP_ORACLE")
COMPRESSED_SAMPLE_ROOT = os.environ.get("PEARE_KWAJ_COMPRESSED_SAMPLE_ROOT")
COMPRESSED_ORACLE_ROOT = os.environ.get("PEARE_KWAJ_COMPRESSED_ORACLE_ROOT")
WORD6_SAMPLE_ROOT = os.environ.get("PEARE_KWAJ_WORD6_SAMPLE_ROOT")
WORD6_ORACLE_ROOT = os.environ.get("PEARE_KWAJ_WORD6_ORACLE_ROOT")


def _assert_direct_payload(peare_lib, sample, expected):
    opener = peare_lib.open_file(str(sample))
    assert opener is not None, (sample, peare_lib.status_message(peare_lib.last_open_status))
    with opener:
        assert opener.container_format() == "KWAJ", sample
        assert opener.folder_count() == 1, sample
        assert opener.resource_count(0) == 1, sample
        resource = po._HANDLE()
        assert peare_lib._lib.peare_opener_open_resource_at(
            opener._h, 0, 0, C.byref(resource)
        ) == 0, sample
        try:
            context = po._Context()
            assert peare_lib._lib.peare_resource_get_context(
                resource, C.byref(context)
            ) == 0, sample
            try:
                assert context.data_size == len(expected), sample
                blob = po._Blob()
                assert peare_lib._lib.peare_resource_get_payload(
                    resource, C.byref(blob)
                ) == 0, sample
                try:
                    actual = blob.to_bytes()
                finally:
                    peare_lib._lib.peare_blob_free(C.byref(blob))
            finally:
                peare_lib._lib.peare_resource_context_free(C.byref(context))
        finally:
            peare_lib._lib.peare_resource_destroy(resource)
        assert hashlib.sha256(actual).digest() == hashlib.sha256(expected).digest(), sample


def test_wfw311_method3_corpus_matches_libmspack(peare_lib):
    if not SAMPLE_ROOT or not ORACLE_ROOT:
        pytest.skip("Set PEARE_KWAJ_SAMPLE_ROOT and PEARE_KWAJ_ORACLE_ROOT")

    sample_root = Path(SAMPLE_ROOT)
    oracle_root = Path(ORACLE_ROOT)
    samples = sorted(
        path for path in sample_root.rglob("*")
        if path.is_file() and path.read_bytes().startswith(b"KWAJ\x88\xf0'\xd1")
    )
    assert len(samples) == 549

    for sample in samples:
        packed = sample.read_bytes()
        assert int.from_bytes(packed[8:10], "little") == 3, sample
        relative = sample.relative_to(sample_root)
        expected_path = oracle_root / relative
        assert expected_path.is_file(), relative
        expected = expected_path.read_bytes()

        _assert_direct_payload(peare_lib, sample, expected)


def test_libmspack_method0_header_corpus_matches_independent_decodes(peare_lib):
    if not UPSTREAM_ROOT or not UPSTREAM_ORACLE_ROOT:
        pytest.skip("Set PEARE_KWAJ_UPSTREAM_SAMPLE_ROOT and PEARE_KWAJ_UPSTREAM_ORACLE_ROOT")

    sample_root = Path(UPSTREAM_ROOT)
    oracle_root = Path(UPSTREAM_ORACLE_ROOT)
    samples = sorted(oracle_root.glob("*.kwj"))
    assert len(samples) == 36
    for expected_path in samples:
        sample = sample_root / expected_path.name
        assert sample.is_file()
        packed = sample.read_bytes()
        assert packed.startswith(b"KWAJ\x88\xf0'\xd1")
        assert int.from_bytes(packed[8:10], "little") == 0
        _assert_direct_payload(peare_lib, sample, expected_path.read_bytes())


def test_libmspack_malformed_header_corpus_is_rejected(peare_lib):
    if not UPSTREAM_ROOT:
        pytest.skip("Set PEARE_KWAJ_UPSTREAM_SAMPLE_ROOT")

    root = Path(UPSTREAM_ROOT)
    invalid = [
        "f04.kwj", "f14.kwj", "f24.kwj", "f34.kwj", "f44.kwj",
        "f54.kwj", "f64.kwj", "f74.kwj", "f84.kwj", "f90.kwj",
        "f91.kwj", "f92.kwj", "f93.kwj", "f94.kwj",
        "cve-2018-14681.kwj", "empty.kwj", "blank.kwj",
    ]
    for name in invalid:
        opener = peare_lib.open_file(str(root / name))
        assert opener is None, name


def test_word6_method4_without_declared_size_matches_libmspack(peare_lib):
    if not MSZIP_SAMPLE or not MSZIP_ORACLE:
        pytest.skip("Set PEARE_KWAJ_MSZIP_SAMPLE and PEARE_KWAJ_MSZIP_ORACLE")

    sample = Path(MSZIP_SAMPLE)
    expected = Path(MSZIP_ORACLE).read_bytes()
    packed = sample.read_bytes()
    assert packed.startswith(b"KWAJ\x88\xf0'\xd1")
    assert int.from_bytes(packed[8:10], "little") == 4
    assert int.from_bytes(packed[12:14], "little") & 1 == 0
    _assert_direct_payload(peare_lib, sample, expected)


def test_cabriolet_methods_0_1_2_match_libmspack(peare_lib):
    if not COMPRESSED_SAMPLE_ROOT or not COMPRESSED_ORACLE_ROOT:
        pytest.skip(
            "Set PEARE_KWAJ_COMPRESSED_SAMPLE_ROOT and PEARE_KWAJ_COMPRESSED_ORACLE_ROOT"
        )

    sample_root = Path(COMPRESSED_SAMPLE_ROOT)
    oracle_root = Path(COMPRESSED_ORACLE_ROOT)
    samples = sorted(sample_root.glob("method-*.kwj"))
    assert len(samples) == 3
    for sample in samples:
        packed = sample.read_bytes()
        method = int(sample.stem.split("-")[1])
        assert method in (0, 1, 2)
        assert int.from_bytes(packed[8:10], "little") == method
        expected = (oracle_root / f"{sample.name}.out").read_bytes()
        _assert_direct_payload(peare_lib, sample, expected)


def test_word6_ms_zip_corpus_matches_libmspack(peare_lib):
    if not WORD6_SAMPLE_ROOT or not WORD6_ORACLE_ROOT:
        pytest.skip("Set PEARE_KWAJ_WORD6_SAMPLE_ROOT and PEARE_KWAJ_WORD6_ORACLE_ROOT")

    sample_root = Path(WORD6_SAMPLE_ROOT)
    oracle_root = Path(WORD6_ORACLE_ROOT)
    samples = sorted(
        path for path in sample_root.rglob("*")
        if path.is_file() and path.read_bytes().startswith(b"KWAJ\x88\xf0'\xd1")
    )
    assert len(samples) == 311
    for sample in samples:
        packed = sample.read_bytes()
        assert int.from_bytes(packed[8:10], "little") == 4, sample
        assert int.from_bytes(packed[12:14], "little") & 1 == 0, sample
        expected_path = oracle_root / sample.relative_to(sample_root)
        assert expected_path.is_file(), sample
        _assert_direct_payload(peare_lib, sample, expected_path.read_bytes())
