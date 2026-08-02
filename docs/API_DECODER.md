# PeareDecoder C API

Header:

```c
#include <peare/peare_decoder.h>
```

The Decoder can be used independently on payloads supplied directly by the consumer.

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

A complete OS/2 `.bmp` file can be passed with both strings set to `NULL`; the Decoder uses structural recognition and RawDetect.

## Text decoding

`peare_decode_texts()` returns complete UTF-8 TXT files. Menus, dialogs, string tables, and message tables can produce one or more items.

## get_info

Available identifiers include:

### Font

- nome face e device;
- copyright;
- codepage;
- primo, ultimo, default e break character;
- fixed width;
- point size e pixel height;
- ascent e descent;
- larghezze media e massima;
- numero di glifi.

### Pointer/cursor

- hotspot X;
- hotspot Y.

### Bitmap

- larghezza;
- altezza;
- BPP;
- compressione.

### Text and resource

- text codepage;
- platform;
- container format;
- tipo;
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

Prima interrogare `get_info()`:

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

A single glyph is a one-character string. The glyph map is not produced by the Decoder; the consumer renders each character and composes the grid.

## Group icon/cursor

A group payload does not contain its child images. A consumer that owns the container must:

1. decode or read the identifiers from the group;
2. retrieve the corresponding `RT_ICON`/`RT_CURSOR` payloads;
3. call `peare_decode_images()` on each child payload.

Resolving child resources is the responsibility of the consumer that owns the container.
