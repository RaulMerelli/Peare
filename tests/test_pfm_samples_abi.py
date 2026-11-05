import ctypes as C
import os
from pathlib import Path

import pytest


SAMPLE_ROOT = Path(os.environ.get(
    "PEARE_PFM_SAMPLE_ROOT",
    Path.home() / "Documents" / "inerente a" / "Peare" /
    "font-samples" / "os2-pfm",
))


class Blob(C.Structure):
    _fields_ = [("bytes", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]


class BlobArray(C.Structure):
    _fields_ = [("items", C.POINTER(Blob)), ("count", C.c_size_t)]


class InfoBuffer(C.Structure):
    _fields_ = [("data", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]


class InfoUnion(C.Union):
    _fields_ = [("boolean", C.c_int), ("signed", C.c_int64),
                ("unsigned", C.c_uint64), ("buffer", InfoBuffer)]


class InfoValue(C.Structure):
    _fields_ = [("type", C.c_int), ("value", InfoUnion)]


def _samples():
    if not SAMPLE_ROOT.is_dir():
        return []
    return [pytest.param(path, id=path.name)
            for path in sorted(SAMPLE_ROOT.glob("*.pfm"))]


@pytest.mark.parametrize("sample", _samples())
def test_os2_pfm_metadata_is_text_through_decoder_abi(
        peare_lib, peare_decoder_path, sample):
    import peare_opener as po

    opener = peare_lib.open_file(str(sample))
    if opener:
        try:
            assert opener.container_format() not in {
                "AFM", "PFM", "TYPE1", "WINDOWS_FNT"
            }
        finally:
            opener.close()

    library = C.CDLL(str(peare_decoder_path))
    library.peare_decode_texts.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        C.POINTER(BlobArray),
    ]
    library.peare_decode_texts.restype = C.c_int
    library.peare_blob_array_free.argtypes = [C.POINTER(BlobArray)]
    library.peare_get_info.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        C.c_int, C.POINTER(InfoValue),
    ]
    library.peare_get_info.restype = C.c_int
    library.peare_value_free.argtypes = [C.POINTER(InfoValue)]
    payload = sample.read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    texts = BlobArray()
    assert library.peare_decode_texts(
        data, len(payload), None, b"RT_FONT", C.byref(texts)
    ) == 0, sample
    try:
        text = "\n".join(C.string_at(texts.items[i].bytes,
                                    texts.items[i].length).decode("utf-8")
                         for i in range(texts.count))
    finally:
        library.peare_blob_array_free(C.byref(texts))
    assert "Printer Font Metrics" in text, sample
    assert "character metrics" in text, sample

    face = InfoValue()
    assert library.peare_get_info(
        data, len(payload), None, b"RT_FONT", 1, C.byref(face)
    ) == 0, sample
    try:
        assert face.type == 4
        assert C.string_at(face.value.buffer.data,
                           face.value.buffer.length).decode("utf-8")
    finally:
        library.peare_value_free(C.byref(face))

    glyph_count = InfoValue()
    assert library.peare_get_info(
        data, len(payload), None, b"RT_FONT", 16, C.byref(glyph_count)
    ) == 0, sample
    try:
        assert glyph_count.value.unsigned > 0
    finally:
        library.peare_value_free(C.byref(glyph_count))
