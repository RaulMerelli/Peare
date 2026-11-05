"""Run the public Opener and Decoder ABIs against real font samples."""

import ctypes as C
import os
from pathlib import Path
import re
import subprocess
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

import pytest
import peare_opener as po

ROOT = Path(__file__).resolve().parent
FIXTURES = ROOT / "abi_fonts" / "fixtures"
FONT_EXTENSIONS = {
    ".afm", ".afb", ".anim", ".bdf", ".bmf", ".dfont", ".dwf", ".eot", ".fon",
    ".4c", ".8c", ".16c", ".afm", ".fb", ".font", ".fzx", ".otf", ".pcf", ".pfb", ".pfa", ".pfm", ".pk", ".psf",
    ".psfu", ".sfn", ".shp", ".shx", ".snf", ".svg", ".ttc", ".ttf",
    ".vfont", ".woff", ".woff2",
}
FONT_CONTAINER_FORMATS = {
    "AFM", "AMIGA_ANIM_FONT", "AMIGA_FONT", "BDF", "BMF", "DFONT", "EOT",
    "FZX", "OTF", "PCF", "PK", "PSF", "SFN", "SHP", "SNF", "TTF", "TYPE1",
    "TYPE3", "VFONT", "WINDOWS_FNT", "WOFF", "WOFF2",
}


class _CodepointArray(C.Structure):
    _fields_ = [
        ("items", C.POINTER(C.c_uint32)),
        ("count", C.c_size_t),
        ("glyph_index_mode", C.c_int),
    ]


class _InfoBuffer(C.Structure):
    _fields_ = [("data", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]


class _InfoValueUnion(C.Union):
    _fields_ = [
        ("boolean", C.c_int),
        ("signed", C.c_int64),
        ("unsigned", C.c_uint64),
        ("buffer", _InfoBuffer),
    ]


class _InfoValue(C.Structure):
    _fields_ = [("type", C.c_int), ("value", _InfoValueUnion)]


class _Blob(C.Structure):
    _fields_ = [("bytes", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]


class _BlobArray(C.Structure):
    _fields_ = [("items", C.POINTER(_Blob)), ("count", C.c_size_t)]


class _FontRenderOptions(C.Structure):
    _fields_ = [
        ("scale", C.c_uint32),
        ("padding", C.c_uint32),
        ("foreground_rgba", C.c_uint32),
        ("background_rgba", C.c_uint32),
    ]


@pytest.fixture(scope="module")
def decoder(peare_decoder_path):
    library = C.CDLL(str(peare_decoder_path))
    library.peare_font_get_codepoints.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        C.POINTER(_CodepointArray),
    ]
    library.peare_font_get_codepoints.restype = C.c_int
    library.peare_font_get_collection_faces.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.POINTER(_BlobArray),
    ]
    library.peare_font_get_collection_faces.restype = C.c_int
    library.peare_font_codepoint_array_free.argtypes = [C.POINTER(_CodepointArray)]
    library.peare_get_info.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p, C.c_int,
        C.POINTER(_InfoValue),
    ]
    library.peare_get_info.restype = C.c_int
    library.peare_value_free.argtypes = [C.POINTER(_InfoValue)]
    library.peare_font_render.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p, C.c_char_p,
        C.POINTER(_FontRenderOptions), C.POINTER(_Blob),
    ]
    library.peare_font_render.restype = C.c_int
    library.peare_blob_free.argtypes = [C.POINTER(_Blob)]
    library.peare_decode_texts.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p,
        C.POINTER(_BlobArray),
    ]
    library.peare_decode_texts.restype = C.c_int
    library.peare_blob_array_free.argtypes = [C.POINTER(_BlobArray)]
    library.peare_font_render_glyph_indexes_with_codepage.argtypes = [
        C.POINTER(C.c_uint8), C.c_size_t, C.c_char_p, C.c_char_p, C.c_uint32,
        C.POINTER(C.c_uint32), C.c_size_t, C.POINTER(_FontRenderOptions),
        C.POINTER(_BlobArray),
    ]
    library.peare_font_render_glyph_indexes_with_codepage.restype = C.c_int
    library.peare_blob_array_free.argtypes = [C.POINTER(_BlobArray)]
    return library


def _render_text(decoder, path, text, scale=1, background_rgba=0xFFFFFFFF):
    payload = path.read_bytes()
    payload_buffer = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    options = _FontRenderOptions(scale, 0, 0x000000FF, background_rgba)
    output = _Blob()
    status = decoder.peare_font_render(
        payload_buffer, len(payload), None, b"RT_FONT",
        text.encode("utf-8"), C.byref(options),
        C.byref(output),
    )
    assert status == 0, path
    try:
        return C.string_at(output.bytes, output.length)
    finally:
        decoder.peare_blob_free(C.byref(output))


def _decode_rgba_png(data):
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    offset = 8
    compressed = bytearray()
    width = height = color_type = bit_depth = None
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4:offset + 8]
        chunk = data[offset + 8:offset + 8 + length]
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, compression, filtering, interlace = \
                struct.unpack(">IIBBBBB", chunk)
            assert (bit_depth, color_type, compression, filtering, interlace) == (8, 6, 0, 0, 0)
        elif chunk_type == b"IDAT":
            compressed.extend(chunk)
        elif chunk_type == b"IEND":
            break
        offset += length + 12
    assert width and height
    raw = zlib.decompress(compressed)
    stride = width * 4
    pixels = bytearray(height * stride)
    source = 0
    for y in range(height):
        filter_type = raw[source]
        source += 1
        row = bytearray(raw[source:source + stride])
        source += stride
        previous = pixels[(y - 1) * stride:y * stride] if y else bytearray(stride)
        for x in range(stride):
            left = row[x - 4] if x >= 4 else 0
            above = previous[x]
            upper_left = previous[x - 4] if x >= 4 else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 0xff
            elif filter_type == 2:
                row[x] = (row[x] + above) & 0xff
            elif filter_type == 3:
                row[x] = (row[x] + ((left + above) >> 1)) & 0xff
            elif filter_type == 4:
                base = left + above - upper_left
                distances = (abs(base - left), abs(base - above), abs(base - upper_left))
                predictor = (left if distances[0] <= distances[1] and distances[0] <= distances[2]
                             else above if distances[1] <= distances[2] else upper_left)
                row[x] = (row[x] + predictor) & 0xff
            else:
                assert filter_type == 0
        pixels[y * stride:(y + 1) * stride] = row
    return width, height, pixels


def _render_font(decoder, path):
    return _render_text(decoder, path, "The quick brown fox jumps over the lazy dog")


def test_cff2_render_fails_promptly_instead_of_blocking(decoder):
    sample = Path(os.path.expandvars(
        r"%TEMP%\peare-font-research\sample-variable-cff2.otf"))
    if not sample.is_file():
        pytest.skip("External CFF2 sample is not available")

    script = r'''
import ctypes as C
import sys

class Options(C.Structure):
    _fields_ = [("scale", C.c_uint32), ("padding", C.c_uint32),
                ("foreground_rgba", C.c_uint32), ("background_rgba", C.c_uint32)]
class Blob(C.Structure):
    _fields_ = [("bytes", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]
class Codepoints(C.Structure):
    _fields_ = [("items", C.POINTER(C.c_uint32)), ("count", C.c_size_t),
                ("glyph_index_mode", C.c_int)]

library = C.CDLL(sys.argv[1])
library.peare_font_get_codepoints.argtypes = [C.POINTER(C.c_uint8), C.c_size_t,
    C.c_char_p, C.c_char_p, C.POINTER(Codepoints)]
library.peare_font_codepoint_array_free.argtypes = [C.POINTER(Codepoints)]
library.peare_font_render.argtypes = [C.POINTER(C.c_uint8), C.c_size_t,
    C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(Options), C.POINTER(Blob)]
library.peare_font_render.restype = C.c_int
library.peare_blob_free.argtypes = [C.POINTER(Blob)]
payload = open(sys.argv[2], "rb").read()
data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
codepoints = Codepoints()
assert library.peare_font_get_codepoints(data, len(payload), None, b"RT_FONT",
    C.byref(codepoints)) == 0
try:
    assert codepoints.count == 99
finally:
    library.peare_font_codepoint_array_free(C.byref(codepoints))
options = Options(1, 0, 0x000000FF, 0xFFFFFFFF)
output = Blob()
status = library.peare_font_render(data, len(payload), None, b"RT_FONT", b"A",
    C.byref(options), C.byref(output))
if output.bytes:
    library.peare_blob_free(C.byref(output))
assert status != 0, status
'''
    result = subprocess.run(
        [sys.executable, "-c", script, decoder._name, str(sample)],
        capture_output=True, text=True, timeout=5,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_local_shx_glyph_batch_does_not_reparse_the_font(decoder):
    sample = os.environ.get("PEARE_SHX_SAMPLE")
    if not sample or not Path(sample).is_file():
        pytest.skip("Set PEARE_SHX_SAMPLE to a real compiled SHX font")
    payload = Path(sample).read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        data, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 0
    try:
        assert codepoints.count > 4096
        indexes = (C.c_uint32 * codepoints.count)(
            *(codepoints.items[index] for index in range(codepoints.count)))
    finally:
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))

    options = _FontRenderOptions(1, 0, 0x000000FF, 0xFFFFFFFF)
    images = _BlobArray()
    status = decoder.peare_font_render_glyph_indexes_with_codepage(
        data, len(payload), None, b"RT_FONT", 0, indexes, len(indexes),
        C.byref(options), C.byref(images))
    assert status == 0
    try:
        assert images.count == len(indexes)
        assert all(C.string_at(images.items[index].bytes, 8) == b"\x89PNG\r\n\x1a\n"
                   for index in range(images.count))
    finally:
        decoder.peare_blob_array_free(C.byref(images))


def test_sfn_collection_faces_are_enumerated_by_decoder(decoder):
    payload = (FIXTURES / "Spleen-collection.sfn").read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    faces = _BlobArray()
    assert decoder.peare_font_get_collection_faces(data, len(payload), C.byref(faces)) == 0
    try:
        assert faces.count == 2
        assert all(faces.items[i].length > 0 for i in range(faces.count))
    finally:
        decoder.peare_blob_array_free(C.byref(faces))


_WINDOWS_TTC_SAMPLES = sorted(
    (Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts").glob("*.ttc"))


@pytest.mark.parametrize(
    "path", _WINDOWS_TTC_SAMPLES[:1] or [None],
    ids=lambda path: path.name if path else "no-local-ttc")
def test_ttc_faces_are_enumerated_by_decoder(decoder, path):
    if path is None:
        pytest.skip("No installed TTC sample is available")
    payload = path.read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    faces = _BlobArray()
    assert decoder.peare_font_get_collection_faces(data, len(payload), C.byref(faces)) == 0
    try:
        assert faces.count > 1
        assert all(faces.items[i].length > 0 for i in range(faces.count))
    finally:
        decoder.peare_blob_array_free(C.byref(faces))


def _png_has_ink(data):
    _, _, pixels = _decode_rgba_png(data)
    return any(pixels[offset] < 128 and pixels[offset + 3] > 0
               for offset in range(0, len(pixels), 4))


def test_vfont_endianness_preserves_bdf_rendering(decoder):
    expected = _render_font(decoder, FIXTURES / "4x6.bdf")
    assert _render_font(decoder, FIXTURES / "4x6.vfontbe") == expected
    assert _render_font(decoder, FIXTURES / "4x6.vfontle") == expected


def test_sfn_contour_counters_remain_open(decoder):
    png = _render_text(decoder, FIXTURES / "Spleen-ascii.sfn", "O", scale=4)
    width, height, pixels = _decode_rgba_png(png)
    center = (height // 2 * width + width // 2) * 4
    assert pixels[center:center + 4] == b"\xff\xff\xff\xff"


def test_local_amiga_colorfont_background_is_transparent(decoder):
    sample = os.environ.get("PEARE_AMIGA_COLOR_FONT_SAMPLE")
    if not sample:
        pytest.skip("Set PEARE_AMIGA_COLOR_FONT_SAMPLE to a real Amiga ColorFont strike")
    png = _render_text(decoder, Path(sample), "T", background_rgba=0xFFFFFF00)
    _, _, pixels = _decode_rgba_png(png)
    assert any(pixels[offset + 3] == 0 for offset in range(0, len(pixels), 4))
    assert any(pixels[offset + 3] == 255 for offset in range(0, len(pixels), 4))
    assert not any(pixels[offset:offset + 4] == b"\x00\x00\x00\xff"
                   for offset in range(0, len(pixels), 4))


def test_local_eot_202_nonzero_eudc_codepage_without_eudc_opens(peare_lib, decoder):
    sample = os.environ.get("PEARE_EOT_CODEPAGE_SAMPLE")
    if not sample:
        pytest.skip("Set PEARE_EOT_CODEPAGE_SAMPLE to an EOT 2.0.2 sample")
    opener = peare_lib.open_file(sample)
    assert opener is not None, sample
    try:
        assert opener.container_format() == "EOT"
        payloads = _font_payloads(opener)
    finally:
        opener.close()
    assert payloads, sample
    assert _decodable_payloads(decoder, payloads) > 0, sample


def test_psf2_unicode_map_omits_unmapped_glyph_slots(decoder):
    path = FIXTURES / "spleen-32x64.psfu"
    payload = path.read_bytes()
    payload_buffer = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        payload_buffer, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 0
    try:
        assert codepoints.glyph_index_mode == 0
        assert codepoints.count == 432
        values = [codepoints.items[index] for index in range(codepoints.count)]
        assert len(set(values)) == len(values)
        assert 0x2584 in values
    finally:
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))


def test_fzx_offsets_are_relative_to_each_character_entry(decoder):
    masks = []
    for name in ("4x6.bdf", "4x6.fzx"):
        width, height, pixels = _decode_rgba_png(
            _render_text(decoder, FIXTURES / name, "A"))
        ink = [(x, y) for y in range(height) for x in range(width)
               if pixels[(y * width + x) * 4] < 128]
        left = min(x for x, _ in ink)
        top = min(y for _, y in ink)
        right = max(x for x, _ in ink)
        bottom = max(y for _, y in ink)
        masks.append(tuple(tuple(
            (x + left, y + top) in ink for x in range(right - left + 1))
            for y in range(bottom - top + 1)))
    assert masks[1] == masks[0]


def test_fzx_duplicate_offsets_preserve_empty_glyphs(decoder):
    png = _render_text(decoder, FIXTURES / "spleen-12x24.fzx", "A")
    width, height, pixels = _decode_rgba_png(png)
    assert width > 0 and height == 24
    assert any(pixels[index] < 128 for index in range(0, len(pixels), 4))


@pytest.mark.parametrize(
    "path", sorted((FIXTURES / "pcf").glob("*.pcf")),
    ids=lambda path: path.name,
)
def test_pcf_variants_preserve_bdf_rendering(decoder, path):
    expected = _render_font(decoder, FIXTURES / "4x6.bdf")
    assert _render_font(decoder, path) == expected


def _spleen_samples():
    formats = {
        ".bdf": "BDF",
        ".dfont": "RESOURCE_FORK",
        ".fzx": "FZX",
        ".fon": "NE",
        ".otf": "OTF",
        ".pcf": "PCF",
        ".psfu": "PSF",
        ".woff": "WOFF",
        ".woff2": "WOFF2",
    }
    return [
        pytest.param(path, formats[path.suffix.lower()], id=path.name)
        for path in sorted(FIXTURES.glob("spleen-*"))
        if path.is_file() and path.suffix.lower() in formats
    ]


def _independent_samples():
    formats = {
        "4x6.bdf": "BDF",
        "4x6.dfont": "RESOURCE_FORK",
        "4x6-sfnt.dfont": "RESOURCE_FORK",
        "4x6.fzx": "FZX",
        "4x6.psf": "PSF",
        "4x6.psfu": "PSF",
        "4x6.ttf": "TTF",
        "4x6.vfontbe": "VFONT",
        "4x6.vfontle": "VFONT",
        "4x6.snf": "SNF",
        "Spleen-ascii.sfn": "SFN",
        "Spleen-latin1.sfn": "SFN",
        "Spleen-collection.sfn": "SFN",
        "6x13.fon": "NE",
        "cmr10.96pk": "PK",
        "hershey.svg": "SVG_FONT",
    }
    samples = [
        pytest.param(FIXTURES / name, expected, id=name)
        for name, expected in formats.items()
    ]
    samples.extend(
        pytest.param(path, "PCF", id=f"pcf/{path.name}")
        for path in sorted((FIXTURES / "pcf").glob("*.pcf"))
    )
    return samples


def _local_snf_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = [Path(item) for item in configured.split(os.pathsep) if item]
    samples = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.snf"):
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if (len(data) >= 108 and data[:4] in {b"\0\0\0\4", b"\4\0\0\0"} and
                    data[104:108] == data[:4]):
                samples.add(path)
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


def _local_dwf_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = [Path(item) for item in configured.split(os.pathsep) if item]
    samples = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.dwf"):
            try:
                header = path.read_bytes()[:6]
            except OSError:
                continue
            if header == b"\x76\x5e\xdf\x01\x54\x00":
                samples.add(path)
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


def _local_sun_news_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = [Path(item) for item in configured.split(os.pathsep) if item]
    samples = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.fb"):
            try:
                with path.open("rb") as source:
                    header = source.read(8)
            except OSError:
                continue
            if len(header) == 8 and header[:4] == b"\x8c\x00\x00\x03":
                samples.add(path)
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


def _local_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    if configured:
        roots = [Path(item) for item in configured.split(os.pathsep) if item]
    else:
        documents = Path.home() / "Documents" / "inerente a"
        roots = [
            Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts",
            documents / "Peare",
            documents / "cerf" / "ProSave_Extracted" /
            "Program Files" / "Common Files" / "Siemens" / "PTProSave" / "AddOn",
            documents / "cerf" / "eVC4" / "OS" / "Fonts",
            Path(os.environ.get("ProgramFiles", "C:/Program Files")) /
            "Adobe" / "Acrobat DC" / "Acrobat" / "ngl_resources" / "resources" /
            "ui" / "font" / "ie",
            Path(os.environ.get("ProgramFiles", "C:/Program Files")) /
            "NVIDIA Corporation" / "NVIDIA GeForce Experience" / "www",
        ]

    paths = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in FONT_EXTENSIONS:
                continue
            if path.stat().st_size == 0:
                continue
            if path.suffix.lower() == ".svg" and not _is_svg_font(path):
                continue
            if path.suffix.lower() == ".afm" and not _is_afm_metrics_sample(path):
                continue
            paths.add(path)
    return [pytest.param(path, id=str(path)) for path in sorted(paths)]


def _local_requested_font_samples():
    extensions = {".4c", ".8c", ".16c", ".afb", ".afm", ".bmf",
                  ".dfont", ".dwf", ".eot", ".fnt", ".pfa", ".pcf", ".pfb", ".pfm", ".font",
                  ".fzx", ".snf", ".svg"}
    samples = []
    for case in _local_samples():
        path = case.values[0]
        extension = path.suffix.lower()
        if extension not in extensions:
            continue
        if extension == ".eot" and not _is_eot_candidate(path):
            continue
        if extension == ".dfont" and not _has_mac_font_resources(path):
            continue
        if extension == ".font" and not _is_amiga_font_index_candidate(path):
            continue
        samples.append(case)
    return samples


def _is_amiga_font_index_candidate(path):
    try:
        data = path.read_bytes()
    except OSError:
        return False
    if len(data) < 4 or int.from_bytes(data[:2], "big") not in {0x0F00, 0x0F02, 0x0F03}:
        return False
    count = int.from_bytes(data[2:4], "big")
    return count <= 4096 and len(data) >= 4 + count * 260


def _has_mac_font_resources(path):
    try:
        data = path.read_bytes()
    except OSError:
        return False
    if len(data) < 30:
        return False
    data_offset, map_offset, data_length, map_length = struct.unpack_from(">IIII", data)
    if (data_offset < 16 or map_offset < 16 or map_length < 30 or
            data_offset + data_length > len(data) or map_offset + map_length > len(data)):
        return False
    if data[map_offset:map_offset + 16] not in (data[:16], bytes(16)):
        return False
    type_offset = int.from_bytes(data[map_offset + 24:map_offset + 26], "big")
    if type_offset < 28 or type_offset + 2 > map_length:
        return False
    count = int.from_bytes(data[map_offset + type_offset:map_offset + type_offset + 2], "big") + 1
    if count > 4096 or type_offset + 2 + count * 8 > map_length:
        return False
    font_types = {b"sfnt", b"FOND", b"NFNT", b"FONT", b"POST"}
    return any(data[map_offset + type_offset + 2 + index * 8:
                    map_offset + type_offset + 6 + index * 8] in font_types
               for index in range(count))


def _is_eot_candidate(path):
    try:
        data = path.read_bytes()
    except OSError:
        return False
    if len(data) < 82 or len(data) > 128 * 1024 * 1024:
        return False
    version = int.from_bytes(data[8:12], "little")
    total_size = int.from_bytes(data[0:4], "little")
    font_size = int.from_bytes(data[4:8], "little")
    if (version not in {0x00010000, 0x00020001, 0x00020002} or
            total_size != len(data) or font_size > total_size - 82 or
            int.from_bytes(data[34:36], "little") != 0x504c or
            data[64:80] != bytes(16) or data[80:82] != b"\0\0"):
        return False
    offset = 80
    for _ in range(4):
        if offset + 4 > len(data) or data[offset:offset + 2] != b"\0\0":
            return False
        length = int.from_bytes(data[offset + 2:offset + 4], "little")
        offset += 4
        if length & 1 or length > len(data) - offset:
            return False
        offset += length
    if version == 0x00010000:
        return True
    if offset + 4 > len(data) or data[offset:offset + 2] != b"\0\0":
        return False
    root_length = int.from_bytes(data[offset + 2:offset + 4], "little")
    offset += 4
    if root_length & 1 or root_length > len(data) - offset:
        return False
    offset += root_length
    if version == 0x00020002:
        if offset + 20 > len(data):
            return False
        if data[offset + 8:offset + 20] != bytes(12):
            return False
    return len(data) - offset - (20 if version == 0x00020002 else 0) == font_size


def _local_type3_samples():
    samples = []
    for case in _local_samples():
        path = case.values[0]
        if path.suffix.lower() != ".pfa":
            continue
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if re.search(rb"/FontType\s+3\b", data):
            samples.append(pytest.param(path, id=str(path)))
    return samples


def _local_type1_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = ([Path(item) for item in configured.split(os.pathsep) if item]
             if configured else [Path.home() / "Documents" / "inerente a" / "Peare"])
    samples = {
        path for root in roots if root.is_dir() for path in root.rglob("*")
        if path.is_file() and path.stat().st_size > 0 and
        path.suffix.lower() == ".pfa"
    }
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


def _local_bmf_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = ([Path(item) for item in configured.split(os.pathsep) if item]
             if configured else [Path.home() / "Documents" / "inerente a" / "Peare"])
    samples = {
        path for root in roots if root.is_dir() for path in root.rglob("*.bmf")
        if path.is_file() and path.stat().st_size > 0
    }
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


@pytest.mark.parametrize("path", _local_bmf_samples())
def test_bmf_codepoint_map_and_sample_text(decoder, path):
    payload = path.read_bytes()
    payload_buffer = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        payload_buffer, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 0
    try:
        assert codepoints.glyph_index_mode == 0
        values = {codepoints.items[index] for index in range(codepoints.count)}
        assert codepoints.count > 0
        if path.name.lower() == "ming.bmf":
            assert all(ord(char) in values for char in "ABCDEFGHIJKLMNOPQRSTUVWXYZ ")
            assert ord("a") not in values
            assert _png_has_ink(_render_text(
                decoder, path, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG"))
    finally:
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))


def _local_quillbold_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = ([Path(item) for item in configured.split(os.pathsep) if item]
             if configured else [Path.home() / "Documents" / "inerente a" / "Peare"])
    samples = {
        path for root in roots if root.is_dir() for path in root.rglob("*.pfb")
        if path.is_file() and path.stat().st_size > 0 and
        path.stem.lower() == "quillbold"
    }
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


@pytest.mark.parametrize("path", _local_quillbold_samples())
def test_type1_pfb_open_counter_renders_as_hole(decoder, path):
    width, height, pixels = _decode_rgba_png(_render_text(decoder, path, "O", scale=4))
    center = (height // 2 * width + width // 2) * 4
    assert pixels[center:center + 4] == b"\xff\xff\xff\xff"


def _local_psf1_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = ([Path(item) for item in configured.split(os.pathsep) if item]
             if configured else [Path.home() / "Documents" / "inerente a" / "Peare"])
    samples = []
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.psf"):
            try:
                if path.stat().st_size >= 4 and path.read_bytes()[:2] == b"\x36\x04":
                    samples.append(path)
            except OSError:
                continue
    return [pytest.param(path, id=str(path)) for path in sorted(set(samples))]


@pytest.mark.parametrize("path", _local_psf1_samples())
def test_psf1_real_sample_renders_through_decoder_abi(decoder, path):
    payload = path.read_bytes()
    payload_buffer = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        payload_buffer, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 0
    try:
        assert codepoints.count in {256, 512}
        if codepoints.glyph_index_mode:
            indexes = (C.c_uint32 * codepoints.count)(
                *range(codepoints.count))
            options = _FontRenderOptions(1, 0, 0x000000FF, 0xFFFFFF00)
            images = _BlobArray()
            assert decoder.peare_font_render_glyph_indexes_with_codepage(
                payload_buffer, len(payload), None, b"RT_FONT", 0, indexes,
                codepoints.count, C.byref(options), C.byref(images)) == 0
            try:
                assert images.count == codepoints.count
                assert any(_png_has_ink(C.string_at(
                    images.items[index].bytes, images.items[index].length))
                    for index in range(images.count))
            finally:
                decoder.peare_blob_array_free(C.byref(images))
        else:
            assert _png_has_ink(_render_font(decoder, path))
    finally:
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))


@pytest.mark.parametrize("path", _local_type1_samples())
def test_type1_glyph_map_batch_renders_real_glyphs(decoder, path):
    payload = path.read_bytes()
    payload_buffer = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        payload_buffer, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 0
    try:
        assert codepoints.glyph_index_mode == 1
        assert codepoints.count > 0
        options = _FontRenderOptions(1, 0, 0x000000FF, 0xFFFFFF00)
        indexes = (C.c_uint32 * codepoints.count)(*range(codepoints.count))
        images = _BlobArray()
        assert decoder.peare_font_render_glyph_indexes_with_codepage(
            payload_buffer, len(payload), None, b"RT_FONT", 0, indexes,
            codepoints.count, C.byref(options), C.byref(images)) == 0
        try:
            assert images.count == codepoints.count
            assert any(_png_has_ink(C.string_at(
                images.items[index].bytes, images.items[index].length))
                for index in range(images.count))
        finally:
            decoder.peare_blob_array_free(C.byref(images))
    finally:
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))


def _local_amiga_strike_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = [Path(item) for item in configured.split(os.pathsep) if item]
    samples = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.stat().st_size < 32 or path.stat().st_size > 64 * 1024 * 1024:
                continue
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if len(data) < 32 or int.from_bytes(data[:4], "big") != 0x3F3:
                continue
            cursor = 4
            for _ in range(4096):
                if cursor + 4 > len(data):
                    break
                name_words = int.from_bytes(data[cursor:cursor + 4], "big")
                cursor += 4
                if name_words == 0:
                    break
                cursor += name_words * 4
            else:
                continue
            if cursor + 16 > len(data):
                continue
            hunk_count = int.from_bytes(data[cursor:cursor + 4], "big")
            cursor += 16
            if not 1 <= hunk_count <= 4096:
                continue
            for _ in range(hunk_count):
                if cursor + 8 > len(data):
                    break
                segment_type = int.from_bytes(data[cursor:cursor + 4], "big") & 0x3FFFFFFF
                segment_words = int.from_bytes(data[cursor + 4:cursor + 8], "big") & 0x3FFFFFFF
                cursor += 8
                segment_size = segment_words * 4
                if segment_size > len(data) - cursor:
                    break
                if segment_type == 0x3E9 and segment_size >= 110:
                    code = data[cursor:cursor + min(segment_size, 110)]
                    if (code[0] == 0x70 and code[2:4] == b"\x4e\x75" and
                            code[12] == 12 and code[18:20] == b"\x0f\x80" and
                            not _is_known_invalid_amiga_strike(path)):
                        samples.add(path)
                    break
                cursor += segment_size
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


def _local_tagged_amiga_contents_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = [Path(item) for item in configured.split(os.pathsep) if item]
    samples = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.font"):
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if (len(data) >= 4 and data[:2] == b"\x0f\x02" and
                    len(data) >= 4 + int.from_bytes(data[2:4], "big") * 260):
                samples.add(path)
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


def _is_known_invalid_amiga_strike(path):
    normalized = path.as_posix().lower()
    return normalized.endswith("/systemfonts3/fonts/gene/8") or \
        normalized.endswith("/systemfonts3/fonts/script/8")


def _local_known_invalid_amiga_strikes():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = [Path(item) for item in configured.split(os.pathsep) if item]
    return [pytest.param(path, id=str(path))
            for root in roots if root.is_dir()
            for path in root.rglob("*")
            if path.is_file() and _is_known_invalid_amiga_strike(path)]


def _local_amiga_anim_samples():
    configured = os.environ.get("PEARE_FONT_SAMPLE_ROOTS", "")
    roots = [Path(item) for item in configured.split(os.pathsep) if item]
    samples = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.stat().st_size < 12:
                continue
            with path.open("rb") as stream:
                header = stream.read(12)
            if header[:4] == b"FORM" and header[8:12] == b"ANIM":
                samples.add(path)
    return [pytest.param(path, id=str(path)) for path in sorted(samples)]


def _local_raw_fnt_samples():
    root = Path.home() / "Documents" / "inerente a" / "Peare" / \
        "software vario" / "w1w2app"
    if not root.is_dir():
        return []
    return [
        pytest.param(path, id=str(path))
        for path in sorted(root.glob("*.fnt"))
        if path.is_file()
    ]


def _local_eot_samples():
    samples = []
    for case in _local_samples():
        path = case.values[0]
        if path.suffix.lower() != ".eot" or not _is_eot_candidate(path):
            continue
        samples.append(pytest.param(path, id=str(path)))
    return samples


def _local_shp_shx_samples():
    return [case for case in _local_samples()
            if case.values[0].suffix.lower() == ".shp" or
            (case.values[0].suffix.lower() == ".shx" and
             _shx_has_font_header(case.values[0]))]


def _local_smcccd_shx_samples():
    return [case for case in _local_shp_shx_samples()
            if case.values[0].name.casefold() == "smcccd-txt.shx"]


def _shx_has_font_header(path):
    data = path.read_bytes()
    marker = data.find(b"\r\n\x1a")
    if marker < 0:
        return True
    offset = marker + 3
    signature = data[:marker]
    stride = 4
    if signature == b"AutoCAD-86 bigfont 1.0":
        if offset + 6 > len(data):
            return True
        count, ranges = struct.unpack_from("<HH", data, offset + 2)
        offset += 6 + ranges * 4
        stride = 8
    elif signature in {b"AutoCAD-86 shapes 1.0", b"AutoCAD-86 shapes 1.1"}:
        if offset + 6 > len(data):
            return True
        count = struct.unpack_from("<H", data, offset + 4)[0]
        offset += 6
    elif signature == b"AutoCAD-86 unifont 1.0":
        if offset + 2 > len(data):
            return True
        count = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        if signature.endswith(b"unifont 1.0"):
            offset = 31 + struct.unpack_from("<H", data, 29)[0]
            return True
    else:
        return True
    if count > 65535 or offset + count * stride > len(data):
        return True
    return any(struct.unpack_from("<H", data, offset + index * stride)[0] == 0
               for index in range(count))


def _local_shx_shape_libraries():
    return [case for case in _local_samples()
            if case.values[0].suffix.lower() == ".shx" and
            not _shx_has_font_header(case.values[0])]


def _local_os2_font_module_samples():
    root = Path.home() / "Documents" / "inerente a" / "Peare"
    old_os2 = root / "IBM OS2 1.20 Extended Edition (3.5-1.44mb)" / \
        "IBM OS2 1.20 Extended Edition (3.5-1.44mb)" / "estratti"
    samples = [
        root / "OS2" / "DLL" / "display.dll",
        old_os2 / "ibmega.dll",
        old_os2 / "ibmcga.dll",
        old_os2 / "ibmbga.dll",
        old_os2 / "ibmvga.dll",
        old_os2 / "pmgre.dll",
    ]
    return [pytest.param(path, id=str(path)) for path in samples if path.is_file()]


def _os2_font_resource_payloads(peare_lib, path):
    opener = peare_lib.open_file(str(path))
    assert opener is not None, path
    marker = b"OS/2 FONT"
    payloads = []
    try:
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = po._HANDLE()
                assert opener._L.peare_opener_open_resource_at(
                    opener._h, folder, index, C.byref(resource)
                ) == 0
                try:
                    blob = po._Blob()
                    assert opener._L.peare_resource_get_payload(
                        resource, C.byref(blob)
                    ) == 0
                    try:
                        payload = blob.to_bytes()
                    finally:
                        opener._L.peare_blob_free(C.byref(blob))
                    position = payload.find(marker)
                    if 8 <= position <= 16:
                        payloads.append(payload)
                finally:
                    opener._L.peare_resource_destroy(resource)
    finally:
        opener.close()
    return payloads


def _microsoft_gzip_font_samples():
    root = Path.home() / "Documents" / "inerente a" / "Peare" / \
        "Microsoft-Fonts-main" / "Microsoft-Fonts-main"
    members = [
        ("1985 - Windows 1.0/COURA.FON.gz", "NE", b"\x00\x01"),
        ("1989 - Windows 2.0/COURE.FON.gz", "NE", b"\x00\x02"),
        ("1993 - Windows NT 3.51/fon/Courier Regular - "
         "c8e431bb8470c268ccf7f8a8e9598281.fon.gz", "NE", b"\x00\x02"),
        ("1995 - Windows 95/fon/Courier Regular - "
         "c22d77530f443114a6a7410761ee24eb.fon.gz", "NE", b"\x00\x02"),
        ("2007 - Windows Home Server/fon/8514fix.fon.gz", "NE", b"\x00\x02"),
        ("2021 - Windows 11/fon/8514fix Regular - "
         "ce4faa4232ca15b59021d809945ddf03.fon.gz", "NE", b"\x00\x02"),
        ("1992 - Windows 3.1/Arial - "
         "cc86004f56acc4f4401e403816d2f688.ttf.gz", "TTF", None),
        ("2021 - Windows 11/ttf/Arial - "
         "fa3228aadde0db988e1822c2f736c131.ttf.gz", "TTF", None),
    ]
    return [
        pytest.param(root / member, expected, header, id=member)
        for member, expected, header in members
        if (root / member).is_file()
    ]


def _is_afm_metrics_sample(path):
    try:
        data = path.read_bytes()
    except OSError:
        return False
    if b"Comment This is not strictly an AFM file." in data:
        return False
    start = data.find(b"StartCharMetrics ")
    end = data.find(b"EndCharMetrics", start)
    if start < 0 or end < 0:
        return False
    return re.search(rb"(?:^|;)\s*(?:WX|W0X)\s", data[start:end]) is not None


def _is_svg_font(path):
    try:
        if path.stat().st_size > 64 * 1024 * 1024:
            return False
        is_svg = False
        in_font = False
        for event, element in ET.iterparse(path, events=("start", "end")):
            name = element.tag.rsplit("}", 1)[-1]
            if event == "start":
                if not is_svg:
                    if name != "svg":
                        return False
                    is_svg = True
                elif name == "font":
                    in_font = True
                elif in_font and name == "font-face":
                    return True
            elif name == "font":
                in_font = False
    except (OSError, ET.ParseError):
        return False
    return False


@pytest.mark.parametrize("sample", _local_shx_shape_libraries())
def test_local_shx_shape_library_is_not_routed_as_a_font(decoder, sample):
    payload = sample.read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        data, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 9, sample


@pytest.mark.parametrize("sample", _local_shp_shx_samples())
def test_local_shp_shx_samples_decode_and_render(decoder, sample):
    payload = sample.read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        data, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 0, sample
    try:
        assert codepoints.count > 0, sample
        assert codepoints.glyph_index_mode == 1, sample
        glyph_index = codepoints.items[0]
    finally:
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))

    options = _FontRenderOptions(1, 1, 0x000000FF, 0xFFFFFFFF)
    image = _Blob()
    assert decoder.peare_font_render_glyph_index_with_codepage(
        data, len(payload), None, b"RT_FONT", 0, glyph_index,
        C.byref(options), C.byref(image)) == 0, sample
    try:
        assert image.bytes and C.string_at(image.bytes, 8) == b"\x89PNG\r\n\x1a\n", sample
    finally:
        decoder.peare_blob_free(C.byref(image))


@pytest.mark.parametrize("sample", _local_smcccd_shx_samples())
def test_smcccd_shx_text_is_rendered_on_one_baseline(decoder, sample):
    png = _render_text(decoder, sample, "The quick brown fox", scale=1)
    width, height = struct.unpack_from(">II", png, 16)
    assert width > 0, sample
    assert height <= 40, (sample, width, height)


@pytest.mark.parametrize("sample", _local_sun_news_samples())
def test_local_sun_news_samples_decode_and_render_all_glyphs(decoder, sample):
    payload = sample.read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        data, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 0, sample
    try:
        assert codepoints.count > 0, sample
        assert codepoints.glyph_index_mode == 1, sample
        indexes = (C.c_uint32 * codepoints.count)(
            *(codepoints.items[index] for index in range(codepoints.count)))
    finally:
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))

    options = _FontRenderOptions(1, 0, 0x000000FF, 0xFFFFFF00)
    images = _BlobArray()
    assert decoder.peare_font_render_glyph_indexes_with_codepage(
        data, len(payload), None, b"RT_FONT", 0, indexes, len(indexes),
        C.byref(options), C.byref(images)) == 0, sample
    try:
        assert images.count == len(indexes), sample
        assert any(_png_has_ink(C.string_at(
            images.items[index].bytes, images.items[index].length))
            for index in range(images.count)), sample
    finally:
        decoder.peare_blob_array_free(C.byref(images))


def _font_payloads(opener):
    payloads = []
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = po._HANDLE()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, C.byref(resource)
            ) == 0
            try:
                context = po._Context()
                assert opener._L.peare_resource_get_context(
                    resource, C.byref(context)
                ) == 0
                resource_type = context.type_utf8.to_bytes().decode("utf-8")
                opener._L.peare_resource_context_free(C.byref(context))
                if resource_type != "RT_FONT":
                    continue
                blob = po._Blob()
                assert opener._L.peare_resource_get_payload(
                    resource, C.byref(blob)
                ) == 0
                try:
                    payloads.append(blob.to_bytes())
                finally:
                    opener._L.peare_blob_free(C.byref(blob))
            finally:
                opener._L.peare_resource_destroy(resource)
    return payloads


def _assert_opener_does_not_classify_font(peare_lib, sample):
    opener = peare_lib.open_file(str(sample))
    if opener is None:
        return
    try:
        assert opener.container_format() not in FONT_CONTAINER_FORMATS
    finally:
        opener.close()


def _resource_payloads(opener):
    payloads = []
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = po._HANDLE()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, C.byref(resource)
            ) == 0
            try:
                blob = po._Blob()
                assert opener._L.peare_resource_get_payload(resource, C.byref(blob)) == 0
                try:
                    payloads.append(blob.to_bytes())
                finally:
                    opener._L.peare_blob_free(C.byref(blob))
            finally:
                opener._L.peare_resource_destroy(resource)
    return payloads


def _decodable_payloads(decoder, payloads, platform=None):
    decoded = 0
    for payload in payloads:
        data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
        codepoints = _CodepointArray()
        status = decoder.peare_font_get_codepoints(
            data, len(payload), platform.encode() if platform else None,
            b"RT_FONT", C.byref(codepoints)
        )
        if status == 0 and codepoints.count:
            decoded += 1
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))
    return decoded


def _decoded_text(decoder, payload):
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    values = _BlobArray()
    if decoder.peare_decode_texts(data, len(payload), None, None,
                                  C.byref(values)) != 0:
        return ""
    try:
        return "\n".join(C.string_at(values.items[index].bytes,
                                     values.items[index].length).decode("utf-8")
                         for index in range(values.count))
    finally:
        decoder.peare_blob_array_free(C.byref(values))


def _font_info(decoder, payload, info_id):
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    value = _InfoValue()
    status = decoder.peare_get_info(
        data, len(payload), None, b"RT_FONT", info_id, C.byref(value)
    )
    if status != 0:
        decoder.peare_value_free(C.byref(value))
        return None
    try:
        if value.type == 3:
            return value.value.unsigned
        if value.type == 4:
            return C.string_at(value.value.buffer.data,
                               value.value.buffer.length).decode("utf-8")
        return None
    finally:
        decoder.peare_value_free(C.byref(value))


def _open_child(parent, name):
    for folder in range(parent.folder_count()):
        for index in range(parent.resource_count(folder)):
            resource = po._HANDLE()
            if parent._L.peare_opener_open_resource_at(
                    parent._h, folder, index, C.byref(resource)) != 0:
                continue
            context = po._Context()
            if parent._L.peare_resource_get_context(
                    resource, C.byref(context)) != 0:
                parent._L.peare_resource_destroy(resource)
                continue
            identifier = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
            is_container = bool(context.is_container)
            parent._L.peare_resource_context_free(C.byref(context))
            if identifier.lower() != name.lower() or not is_container:
                parent._L.peare_resource_destroy(resource)
                continue
            source = po._HANDLE()
            status = parent._L.peare_resource_get_source(resource, C.byref(source))
            parent._L.peare_resource_destroy(resource)
            if status != 0:
                return None
            child = parent._lib._open_source(source)
            parent._L.peare_source_destroy(source)
            return child
    return None


@pytest.mark.parametrize("sample,header", [
    (FIXTURES / "spleen-8x16.psfu", b"\x36\x04"),
    (FIXTURES / "4x6.psf", b"\x72\xb5\x4a\x86"),
    (FIXTURES / "4x6.psfu", b"\x72\xb5\x4a\x86"),
])
def test_psf_version_samples_have_expected_headers(sample, header):
    assert sample.read_bytes().startswith(header)


@pytest.mark.parametrize("sample,expected_format", _spleen_samples())
def test_spleen_upstream_font_samples_open(peare_lib, sample, expected_format):
    opener = peare_lib.open_file(str(sample))
    if expected_format not in {"NE", "RESOURCE_FORK"}:
        if opener is not None:
            try:
                assert opener.container_format() not in FONT_CONTAINER_FORMATS
            finally:
                opener.close()
        return
    assert opener is not None, sample
    try:
        assert opener.container_format() == expected_format
        assert opener.folder_count() > 0
        if expected_format == "NE":
            assert _font_payloads(opener), f"No embedded RT_FONT resource in {sample}"
        else:
            assert _resource_payloads(opener), f"No resource fork entries in {sample}"
    finally:
        opener.close()


@pytest.mark.parametrize("sample,expected_format", _spleen_samples())
def test_spleen_upstream_font_samples_decode(peare_lib, decoder, sample,
                                            expected_format):
    if expected_format == "NE":
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample
        try:
            payloads = _font_payloads(opener)
        finally:
            opener.close()
    elif expected_format == "RESOURCE_FORK":
        opener = peare_lib.open_file(str(sample))
        assert opener is not None, sample
        try:
            payloads = _resource_payloads(opener)
        finally:
            opener.close()
    else:
        payloads = [sample.read_bytes()]
    assert _decodable_payloads(decoder, payloads) > 0, sample


@pytest.mark.parametrize("sample,expected_format", _independent_samples())
def test_independent_font_samples_open_and_decode(peare_lib, decoder, sample,
                                                  expected_format):
    opener = peare_lib.open_file(str(sample))
    if expected_format not in {"NE", "RESOURCE_FORK"}:
        if opener is not None:
            try:
                assert opener.container_format() not in FONT_CONTAINER_FORMATS
            finally:
                opener.close()
        if expected_format == "SNF":
            assert _font_info(decoder, sample.read_bytes(), 1)
            return
        assert _decodable_payloads(decoder, [sample.read_bytes()]) > 0, sample
        return
    assert opener is not None, sample
    try:
        assert opener.container_format() == expected_format
        payloads = (_font_payloads(opener) if expected_format == "NE"
                    else _resource_payloads(opener))
        assert payloads, f"No raw resources in {sample}"
    finally:
        opener.close()
    if expected_format == "SNF":
        assert _font_info(decoder, payloads[0], 1) == (
            "-Misc-Fixed-Medium-R-Normal--6-60-75-75-C-40-ISO10646-1"
        )
        assert _font_info(decoder, payloads[0], 5) == 0
        assert _font_info(decoder, payloads[0], 6) == 65535
    else:
        assert _decodable_payloads(decoder, payloads) > 0, sample


@pytest.mark.parametrize("sample", _local_snf_samples())
def test_local_snf_samples_expose_metadata_via_abi(peare_lib, decoder, sample):
    _assert_opener_does_not_classify_font(peare_lib, sample)
    payload = sample.read_bytes()
    face_name = _font_info(decoder, payload, 1)
    first = _font_info(decoder, payload, 5)
    last = _font_info(decoder, payload, 6)
    assert isinstance(face_name, str) and face_name
    assert isinstance(first, int) and isinstance(last, int) and first <= last


@pytest.mark.parametrize("sample", _local_dwf_samples())
def test_local_dwf_samples_decode_and_render_via_abi(peare_lib, decoder, sample):
    _assert_opener_does_not_classify_font(peare_lib, sample)
    payload = sample.read_bytes()
    data = (C.c_uint8 * len(payload)).from_buffer_copy(payload)
    codepoints = _CodepointArray()
    assert decoder.peare_font_get_codepoints(
        data, len(payload), None, b"RT_FONT", C.byref(codepoints)) == 0, sample
    try:
        assert codepoints.count > 0, sample
        indexes = (C.c_uint32 * codepoints.count)(
            *(codepoints.items[index] for index in range(codepoints.count)))
    finally:
        decoder.peare_font_codepoint_array_free(C.byref(codepoints))
    options = _FontRenderOptions(1, 0, 0x000000FF, 0xFFFFFF00)
    images = _BlobArray()
    assert decoder.peare_font_render_glyph_indexes_with_codepage(
        data, len(payload), None, b"RT_FONT", 0, indexes, len(indexes),
        C.byref(options), C.byref(images)) == 0, sample
    try:
        assert images.count == len(indexes), sample
        assert any(_png_has_ink(C.string_at(
            images.items[index].bytes, images.items[index].length))
            for index in range(images.count)), sample
    finally:
        decoder.peare_blob_array_free(C.byref(images))


def test_amiga_font_contents_is_decoded_without_opener(peare_lib, decoder):
    sample = FIXTURES / "wbfont_prop.font"
    _assert_opener_does_not_classify_font(peare_lib, sample)
    assert "font contents" in _decoded_text(decoder, sample.read_bytes()).lower()


@pytest.mark.parametrize("sample", _local_tagged_amiga_contents_samples())
def test_local_tagged_amiga_font_contents_open(peare_lib, sample):
    _assert_opener_does_not_classify_font(peare_lib, sample)


@pytest.mark.parametrize("sample", _local_amiga_strike_samples())
def test_local_amiga_strikes_open(peare_lib, sample):
    _assert_opener_does_not_classify_font(peare_lib, sample)


@pytest.mark.parametrize("sample", _local_known_invalid_amiga_strikes())
def test_known_invalid_amiga_strikes_are_rejected(peare_lib, sample):
    assert peare_lib.open_file(str(sample)) is None


@pytest.mark.parametrize("sample", _local_amiga_anim_samples())
def test_local_amiga_anim_fonts_open(peare_lib, sample):
    assert peare_lib.open_file(str(sample)) is None


@pytest.mark.parametrize("sample_name", ["bende.sfn"])
def test_sfn_gzip_wrapped_sample_opens_and_decodes(peare_lib, decoder,
                                                   sample_name):
    sample = FIXTURES / sample_name
    outer = peare_lib.open_file(str(sample))
    assert outer is not None, sample
    try:
        assert outer.container_format() == "GZIP"
        payloads = _resource_payloads(outer)
    finally:
        outer.close()
    assert payloads, f"No decompressed payload in {sample}"
    assert _decodable_payloads(decoder, payloads) > 0, sample


@pytest.mark.parametrize("sample,expected_format,expected_header",
                         _microsoft_gzip_font_samples())
def test_local_gzip_wrapped_font_samples_open_and_decode(
        peare_lib, decoder, sample, expected_format, expected_header):
    outer = peare_lib.open_file(str(sample))
    assert outer is not None, sample
    try:
        assert outer.container_format() == "GZIP"
        if expected_format == "NE":
            child = _open_child(outer, sample.name[:-3])
            assert child is not None, sample
            try:
                assert child.container_format() == "NE"
                payloads = _font_payloads(child)
            finally:
                child.close()
        else:
            payloads = _resource_payloads(outer)
    finally:
        outer.close()
    assert payloads, f"No decompressed payload in {sample}"
    if expected_header and expected_format != "NE":
        assert any(payload.startswith(expected_header) for payload in payloads)
    assert _decodable_payloads(decoder, payloads) > 0, sample


def test_local_arcaos_font_zip_sample_opens_and_decodes(peare_lib, decoder):
    archive = Path.home() / "Documents" / "inerente a" / "Peare" / \
        "ArcaOS-5.0" / "OS2IMAGE" / "FI" / "FONT" / "tnrwt_j.zip"
    if not archive.is_file():
        pytest.skip("Local ArcaOS font package is not available")
    opener = peare_lib.open_file(str(archive))
    assert opener is not None
    try:
        payloads = _resource_payloads(opener)
    finally:
        opener.close()
    payloads = [payload for payload in payloads
                if _decodable_payloads(decoder, [payload]) > 0]
    assert payloads
    assert _decodable_payloads(decoder, payloads) > 0


@pytest.mark.parametrize("sample", _local_requested_font_samples())
def test_local_requested_font_samples_open(peare_lib, sample):
    opener = peare_lib.open_file(str(sample))
    if sample.suffix.lower() == ".dfont":
        assert opener is not None, sample
        try:
            assert opener.container_format() == "RESOURCE_FORK"
        finally:
            opener.close()
    else:
        if opener is not None:
            try:
                assert opener.container_format() not in FONT_CONTAINER_FORMATS
            finally:
                opener.close()


@pytest.mark.parametrize(
    "sample", [case for case in _local_requested_font_samples()
               if case.values[0].suffix.lower() == ".dfont"])
def test_local_dfont_postscript_samples_open_and_decode(peare_lib, decoder, sample):
    opener = peare_lib.open_file(str(sample))
    assert opener is not None, sample
    try:
        assert opener.container_format() == "RESOURCE_FORK"
        payloads = _resource_payloads(opener)
    finally:
        opener.close()
    assert payloads, f"No raw resource records in {sample}"
    decoded = _decodable_payloads(decoder, payloads)
    if not decoded:
        decoded = _decodable_payloads(decoder, [sample.read_bytes()])
    assert decoded > 0, sample


@pytest.mark.parametrize("sample", _local_type3_samples())
def test_local_type3_fonts_open_and_decode(peare_lib, decoder, sample):
    assert peare_lib.open_file(str(sample)) is None
    assert _decodable_payloads(decoder, [sample.read_bytes()]) > 0, sample


@pytest.mark.parametrize("sample", _local_raw_fnt_samples())
def test_local_raw_fnt_samples_decode(decoder, sample):
    payload = sample.read_bytes()
    assert payload.startswith(b"\x00\x01"), sample
    assert _decodable_payloads(decoder, [payload], "Windows") > 0, sample


@pytest.mark.parametrize("sample", _local_eot_samples())
def test_local_eot_samples_open_and_decode(peare_lib, decoder, sample):
    assert peare_lib.open_file(str(sample)) is None
    assert _decodable_payloads(decoder, [sample.read_bytes()]) > 0, sample


@pytest.mark.parametrize("sample", _local_os2_font_module_samples())
def test_local_os2_fnt_payloads_in_modules_decode(peare_lib, decoder, sample):
    payloads = _os2_font_resource_payloads(peare_lib, sample)
    assert payloads, f"No OS/2 FONT resource payload in {sample}"
    assert _decodable_payloads(decoder, payloads, "OS/2") > 0, sample
