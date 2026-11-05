# PeareDecoder C API

Header:

```c
#include <peare/peare_decoder.h>
```

The Decoder accepts payloads supplied by the caller. It can run without the
Opener.

Every main function receives the same basic input:

```text
payload, length, optional platform, optional resource type
```

Platform examples: `"Windows"`, `"OS/2"`, `"Other"`.

Type examples: `"RT_BITMAP"`, `"RT_FONT"`, `"RT_MENU"`.

Pass `NULL` or an empty string to request recognition from the payload alone.

## Exports

```c
peare_status peare_decode_images(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_blob_array *out_pngs);

typedef peare_status (*peare_resource_resolver_fn)(
    void *user_data,
    const char *resource_type_utf8,
    size_t resource_type_length,
    const char *identifier_utf8,
    size_t identifier_length,
    const uint8_t **out_payload,
    size_t *out_payload_length);

peare_status peare_decode_images_with_resolver(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_resource_resolver_fn resolver,
    void *resolver_user_data,
    peare_blob_array *out_pngs);

peare_status peare_decode_texts(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_blob_array *out_texts);

peare_status peare_get_info(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    peare_info_id info_id,
    peare_value *out_value);

peare_status peare_get_item_info(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    size_t item_index,
    peare_info_id info_id,
    peare_value *out_value);

peare_status peare_font_render(
    const uint8_t *payload,
    size_t payload_length,
    const char *platform_utf8,
    const char *resource_type_utf8,
    const char *text_utf8,
    const peare_font_render_options *options,
    peare_blob *out_png);

peare_status peare_font_get_collection_faces(
    const uint8_t *payload,
    size_t payload_length,
    peare_blob_array *out_faces);

void peare_blob_free(peare_blob *blob);
void peare_blob_array_free(peare_blob_array *array);
void peare_value_free(peare_value *value);
```

## Image decoding

`peare_decode_images()` returns zero or more complete PNG files in memory.

```c
peare_blob_array pngs = {0};
peare_status st = peare_decode_images(
    data, size, "OS/2", "RT_BITMAP", &pngs);

if (st == PEARE_STATUS_OK) {
    for (size_t i = 0; i < pngs.count; ++i) {
        const uint8_t *png = pngs.items[i].bytes;
        size_t png_size = pngs.items[i].length;
        /* use or save the PNG */
    }
}

peare_blob_array_free(&pngs);
```

A complete OS/2 `.bmp` file can use `NULL` for both strings.
The Decoder then uses structural recognition and RawDetect.

### Doom WAD graphics

`peare_decode_images_with_resolver()` supports decoders that need sibling
resources. For WAD resources, it decodes `WAD_DOOM_PATCH` and
`WAD_DOOM_SPRITE`, and builds standard `TEXTURE1` and `TEXTURE2` images from
`WAD_TEXTUREX` payloads. The resolver receives the requested resource type and
identifier. Its returned bytes must remain valid until the call returns. Return
`PEARE_STATUS_NOT_FOUND` when a requested resource is not present.

The decoder uses the first 256-color palette in `PLAYPAL`. It does not decode
ZDoom `TEXTURES` definitions or flat lumps.

## Text decoding

`peare_decode_texts()` returns complete UTF-8 TXT files. Menus, dialogs, string tables, and message tables can produce one or more items.

## get_info

Available identifiers include:

### Font

- face and device name.
- copyright.
- code page.
- first, last, default, and break characters.
- fixed width.
- point size and pixel height.
- ascent and descent.
- average and maximum width.
- glyph count.

### Pointer/cursor

- hotspot X;
- hotspot Y.

### Bitmap

- width;
- height;
- bits per pixel;
- compression type.

### Text and resource

- text codepage;
- platform;
- container format;
- type;
- language.

Origin data may not be determinable when the Decoder is used standalone because the ABI receives only the two optional strings. An explicit status is returned in that case.

## get_item_info

Use for multiple outputs, for example OS/2 bitmap arrays or pointer arrays:

```c
peare_value value = {0};
peare_status st = peare_get_item_info(
    payload, length, "OS/2", "RT_BITMAP",
    2, PEARE_INFO_BITMAP_BPP, &value);

peare_value_free(&value);
```

The index matches the order of the items returned by `peare_decode_images()`.

## Font

The Decoder accepts standalone font bytes and raw bytes obtained from any
container resource. The Opener does not identify or classify font payloads.
`peare_font_get_collection_faces()` returns the individual payloads in a TTC or
SFNC collection. Free the returned array with `peare_blob_array_free()`.

Font decoding supports Windows and OS/2 FNT, BDF/AFB, VFONT, BMF 1.1/1.2,
PCF, PSF1/PSF2, SNF metadata, FZX, TeX PK, AFM metadata, Type 1, Type 3, SVG
Font, SFN/SFNC, TrueType/TTC, OpenType, WOFF, WOFF2, EOT, SHP/SHX, Amiga
bitmap and AnimFont data, and font records in Apple resource forks. AFM and SNF
provide metadata but do not render glyphs. The Decoder does not change input
bytes. It expands supported WOFF data in memory for rendering.
The caller must create a `QGuiApplication` before it requests outline-font
rendering. Bitmap-font rendering does not have this requirement.

First, query `get_info()`:

```c
peare_value first = {0};
peare_value last = {0};

peare_status a = peare_get_info(
    payload, length, platform, type,
    PEARE_INFO_FONT_FIRST_CHARACTER, &first);

peare_status b = peare_get_info(
    payload, length, platform, type,
    PEARE_INFO_FONT_LAST_CHARACTER, &last);
```

When the type is unknown and the first query succeeds, the consumer can use `"RT_FONT"` in subsequent calls. This avoids repeating RawDetect.

Rendering:

```c
peare_font_render_options options = {
    2,          /* scale */
    2,          /* padding */
    0xFFFFFFFF, /* foreground RGBA */
    0x00000000  /* background RGBA */
};

peare_blob png = {0};
peare_status st = peare_font_render(
    payload, length, platform, "RT_FONT",
    "Hello world", &options, &png);

peare_blob_free(&png);
```

`peare_font_get_codepoints()` returns either Unicode codepoints or glyph indexes.
When `glyph_index_mode` is zero, the array contains Unicode codepoints and omits
glyphs without a character mapping. When it is nonzero, the array contains every
glyph index from zero to `PEARE_INFO_FONT_GLYPH_COUNT - 1`. Free it with
`peare_font_codepoint_array_free()`.

Use `peare_font_supports_text_with_codepage()` to check if a font supports a text
string. It returns `1` in `out_supported` when the font supports every non-space
character. It returns `0` when one or more characters are missing.

```c
int supported = 0;
peare_status st = peare_font_supports_text_with_codepage(
    payload, length, platform, "RT_FONT", code_page, "Hello world", &supported);
```

When `glyph_index_mode` is nonzero, use `peare_font_render_glyph_index()` to
render the full glyph table. Peare supports glyph-index rendering for bitmap,
SFNT, SFN, SHP, Amiga, and Type 1/3 fonts. Otherwise, render each array entry
with `peare_font_render()`.

When `glyph_index_mode` is zero, the glyph count from `get_info()` can be larger
because it includes glyphs without Unicode mappings. Use glyph-index rendering
with the complete index range to include those glyphs.

For scalable SFNT fonts, render glyphs by index to include glyphs that have no
Unicode mapping:

```c
peare_blob glyph = {0};
peare_status st = peare_font_render_glyph_index(
    payload, length, platform, "RT_FONT", glyph_index, &options, &glyph);

peare_blob_free(&glyph);
```

`glyph_index` must be less than the count returned by `PEARE_INFO_FONT_GLYPH_COUNT`.
SFNT outline rendering needs a `QGuiApplication`. Bitmap, SFN, and SHP glyph
rendering does not need one.

For a glyph grid, pass all glyph indexes to `peare_font_render_glyph_indexes_with_codepage()`.
The function returns one PNG for each requested index. Free the result with
`peare_blob_array_free()`. The function parses SFNT data once for the full batch.

```c
peare_blob_array glyphs = {0};
peare_status st = peare_font_render_glyph_indexes_with_codepage(
    payload, length, platform, "RT_FONT", code_page, indexes, count, &options, &glyphs);
peare_blob_array_free(&glyphs);
```

### Packed SHP/SHX code pages

UNIFONT encoding 1 stores packed character codes. These codes need the code page
from the drawing that uses the font. The original font file does not store it.

Use the `*_with_codepage()` font functions when the caller knows this code page.
The functions map packed codes to Unicode for character listing and text rendering.
If a code page is missing or a code is invalid, the functions return packed glyph
IDs instead. For Packed SHP/SHX, the existing font functions always use this
glyph-index mode.

The Peare GUI passes the code page stored in the selected resource context. Standalone
font files without a code page show the glyph IDs.

## Group icon/cursor

A group payload does not contain its child images. A consumer that owns the container must:

1. Decode or read the identifiers from the group.
2. Retrieve the matching `RT_ICON` or `RT_CURSOR` payloads.
3. call `peare_decode_images()` on each child payload.

Resolving child resources is the responsibility of the consumer that owns the container.
