import json
import os
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
SAMPLES = ROOT / "tmp" / "font-sisla-investigation"
FONT_NAMES = ("SISLA08.TTF", "SISLA33.TTF")
ABI_PROBE = r'''
import ctypes as C
import json
import sys
from pathlib import Path

class Codepoints(C.Structure):
    _fields_ = [("items", C.POINTER(C.c_uint32)), ("count", C.c_size_t),
                ("glyph_index_mode", C.c_int)]
class Blob(C.Structure):
    _fields_ = [("bytes", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]
class Options(C.Structure):
    _fields_ = [("scale", C.c_uint32), ("padding", C.c_uint32),
                ("foreground_rgba", C.c_uint32), ("background_rgba", C.c_uint32)]

library = C.CDLL(sys.argv[1])
library.peare_font_get_codepoints.argtypes = [C.POINTER(C.c_uint8), C.c_size_t,
    C.c_char_p, C.c_char_p, C.POINTER(Codepoints)]
library.peare_font_get_codepoints.restype = C.c_int
library.peare_font_codepoint_array_free.argtypes = [C.POINTER(Codepoints)]
library.peare_font_render.argtypes = [C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p,
    C.c_char_p, C.c_char_p, C.POINTER(Options), C.POINTER(Blob)]
library.peare_font_render.restype = C.c_int
library.peare_blob_free.argtypes = [C.POINTER(Blob)]
payload = Path(sys.argv[2]).read_bytes()
data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
codepoints = Codepoints()
status = library.peare_font_get_codepoints(data, len(payload), None, b"RT_FONT",
                                           C.byref(codepoints))
if status or not codepoints.count:
    raise SystemExit(f"codepoint query failed: {status}, {codepoints.count}")
count = codepoints.count
library.peare_font_codepoint_array_free(C.byref(codepoints))
options = Options(1, 0, 0x000000FF, 0xFFFFFFFF)
image = Blob()
status = library.peare_font_render(data, len(payload), None, b"RT_FONT",
    b"The quick brown fox jumps over the lazy dog", C.byref(options), C.byref(image))
if status or not image.bytes or image.length < 24:
    raise SystemExit(f"font render failed: {status}, {image.length}")
png = C.string_at(image.bytes, 8)
image_size = image.length
library.peare_blob_free(C.byref(image))
if png != b"\x89PNG\r\n\x1a\n":
    raise SystemExit("font render returned invalid PNG data")
print(json.dumps({"glyphs": count, "png_bytes": image_size}))
'''


@pytest.mark.parametrize("font_name", FONT_NAMES)
def test_local_siemens_sisla_font_works_through_decoder_abi(
        peare_decoder_path, font_name):
    sample = SAMPLES / font_name
    if not sample.is_file():
        pytest.skip(f"Local Peare-extracted sample is not available: {sample}")
    environment = dict(os.environ)
    environment["PATH"] = str(peare_decoder_path.parent) + os.pathsep + environment.get("PATH", "")
    result = subprocess.run(
        [sys.executable, "-c", ABI_PROBE, str(peare_decoder_path), str(sample)],
        capture_output=True, text=True, env=environment, timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    report = json.loads(result.stdout)
    assert report["glyphs"] > 0
    assert report["png_bytes"] > 24
