# Architecture

## Objective

Peare separates three responsibilities:

```text
Peare GUI
├── PeareOpener.dll
└── PeareDecoder.dll
```

### PeareOpener.dll

Stateful component that:

- opens files or buffers;
- recognizes DOS MZ, PE, NE, LE, LX, XBE, XEX1/XEX2, XUIZ/XZP, LIVE, PIRS, and CON;
- maintains a container session;
- lists folders, types, and resources;
- searches for a resource;
- reconstructs the byte-perfect original payload;
- returns its origin context.

It does not decode images, fonts, menus, dialogs, or text. Whole-module transformations, such as PE loaded-image to file-layout conversion, remain in the Opener.

### PeareDecoder.dll

Standalone component that receives directly:

```text
payload + optional platform string + optional resource-type string
```

And produces:

- PNG arrays;
- UTF-8 TXT arrays;
- typed information;
- PNG files produced by font rendering.

It does not open containers; it receives payloads and textual context directly from the consumer.

### Peare.exe

The GUI:

- opens and navigates containers through the Opener;
- retrieves payloads and context;
- passes payloads and context strings to the Decoder;
- displays PNG and TXT output;
- queries `get_info()` and `get_item_info()`;
- composes the glyph map by repeatedly calling `peare_font_render()`;
- exports original payloads and converted output;
- displays a hexadecimal dump as the application fallback.

## Main flow

```text
PeareOpener.dll
  open file/buffer
  list folders/resources
  open resource
       ↓
  payload byte-perfect + context
       ↓
Peare.exe
  converts context.platform to "Windows" / "OS/2" / "Other"
  uses context.type as an RT_* string
       ↓
PeareDecoder.dll
  decode_images / decode_texts / get_info / font_render
```

## Decoder routing

The Decoder uses, in order:

1. the textual resource type, when present;
2. the textual platform, when present;
3. the payload structure;
4. RawDetect;
5. heuristic fallback.

Type examples:

- `RT_BITMAP`
- `RT_ICON`
- `RT_CURSOR`
- `RT_FONT`
- `RT_MENU`
- `RT_DIALOG`
- `RT_STRING`
- `RT_MESSAGE`

The strings may be `NULL` or empty. In that case, the Decoder attempts recognition from the payload alone.

## Related resources

`RT_GROUP_ICON` and `RT_GROUP_CURSOR` contain descriptors and identifiers, not the pixels of their child images. The GUI uses the Opener to resolve `RT_ICON`/`RT_CURSOR` identifiers, then passes each child payload to the Decoder. The consumer resolves child resources before invoking the Decoder.

## Font

The Decoder exposes a single renderer:

```text
font + text + options → PNG
```

The GUI:

1. queries `PEARE_INFO_FONT_FIRST_CHARACTER` and `PEARE_INFO_FONT_LAST_CHARACTER`;
2. this also recognizes fonts identified by RawDetect;
3. uses the effective `RT_FONT` type for subsequent calls;
4. renders the sentence at 1x, 2x, 3x, and 4x;
5. renders each character at 1x;
6. composes the glyph map.

## ABI boundaries

Public headers do not expose:

- `QString`;
- `QByteArray`;
- `QImage`;
- STL;
- C++ classes;
- C++ exceptions.

Exports are undecorated C symbols controlled by the public ABI declarations and platform visibility macros.


## XEX1/XEX2 support

`XexModule` owns XEX1/XEX2 container parsing and delegates Windows resource-tree parsing to `PeResources::list(const QByteArray&)`. The initial implementation accepts an embedded, directly readable PE image. Encryption and XEX compression are intentionally isolated for later loaders and do not affect PE/NE/LE/LX modules.

## Original Xbox XBE

`XbeModule` validates the `XBEH` image header, translates virtual addresses relative to the image base, parses the certificate and UTF-16 title, and exposes certificate bytes, a textual metadata summary, the embedded compressed logo payload, and each valid section as independent resources. Decoding the Xbox logo RLE stream and interpreting native resources inside sections are Decoder responsibilities. The Opener exposes only the original `XBE_LOGO_RLE` payload.

### XBE extended metadata

The XBE module also exposes the TLS structure, linked library-version records,
kernel/XAPI version records, decoded retail/debug kernel thunk tables, and the
non-kernel import directory as independent resources. Kernel imports are kept
as ordinal records because symbol names depend on the target Xbox kernel build.

## XBE embedded-resource discovery

`XbeModule` performs a conservative second pass over validated XBE section data.
It exposes valid XPR0/XPR1/XPR2 packages as `XPR_PACKAGE` and valid DDS headers as
`DDS_IMAGE`, while preserving the parent section. Discovery never scans outside
the section and rejects malformed XPR size/header combinations.

### XBE integrity metadata

The opener exposes the original 256-byte XBE header signature and each section's
stored SHA-1 digest. It recomputes SHA-1 over the bounded raw section and reports
`valid`, `mismatch`, or `not present` in `XBE_SECTION_METADATA`. RSA authenticity
is not claimed because it depends on the matching Xbox public key.

## Native module tree hierarchy

The opener does not add synthetic format roots such as `PE structure`, `Sections`,
or `Resources`. Structural units native to the executable are first-level nodes.
PE sections are direct children of the module; PE resources are children of the
`.rsrc` section. NE exposes its loadable code and data segments as logical root
areas (`.text`, `.data`, with numeric suffixes only when the format contains more
than one area of the same class), while keeping segment indices internal. LE and
LX classify non-resource objects from their protection flags: executable objects
become `.text`, initialized non-executable objects become `.data`, and objects
without physical pages become `.bss`. Objects flagged as resources are not shown
twice: their resource-table entries are children of the single `.rsrc` root.
Headers are a root-level selectable payload for every executable format. XBE
embedded payloads remain below their containing named section. This rule applies
equally to standalone modules and modules embedded in another container.

## Xbox 360 STFS modules

`LIVE`, `PIRS`, and `CON` are archive/container formats and are owned by PeareOpener rather than PeareDecoder. `LivePirsModule` and `ConModule` expose the original package, metadata, icons, directories, and contained files as opener resources. Their shared STFS parsing code is private to `src/opener/modules/stfs`.

All leaf resource decoders, including FMIM and Windows/OS2 resource types, live under `src/decoder/resources`.
