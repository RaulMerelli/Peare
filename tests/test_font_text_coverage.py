"""Test font text coverage through the public Decoder ABI."""

import ctypes as C
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SAMPLE = ROOT / "abi_fonts" / "fixtures" / "4x6.ttf"


def test_sfnt_text_coverage_uses_decoder_abi(peare_decoder_path):
    decoder = C.CDLL(str(peare_decoder_path))
    check = decoder.peare_font_supports_text_with_codepage
    check.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p, C.c_uint32,
        C.c_char_p, C.POINTER(C.c_int),
    ]
    check.restype = C.c_int
    payload = SAMPLE.read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    supported = C.c_int()

    assert check(data, len(payload), None, b"RT_FONT", 0, b"The quick brown fox",
                 C.byref(supported)) == 0
    assert supported.value == 1
    assert check(data, len(payload), None, b"RT_FONT", 0, "漢字".encode("utf-8"),
                 C.byref(supported)) == 0
    assert supported.value == 0
