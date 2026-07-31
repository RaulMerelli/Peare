# Peare

*Plugin-Extendable Advanced Resource Editor*

<p align="center">
  <img src="resources/peare.png" alt="Peare icon" width="192">
</p>

<p align="center">
  <img src="resources/preview.gif" alt="Peare icon" width="192">
</p>

Peare is a resource explorer, decoder, and future resource editor for historical executable formats and application packages.

The project is inspired by **Resource Hacker** and **BCC Workshop**. Its current focus is understanding and preserving resources from classic Windows and OS/2 software, including formats that modern tools often ignore or only partially support.

## Why the name Peare?

**Peare** is an acronym for **Plugin-Extendable Advanced Resource Editor**.

The name describes the long-term direction of the project. The current implementation concentrates on opening containers, listing resources, preserving their original payloads, and decoding them. Editing and the plugin system remain future work.

## Project history

This project was born by the merging of three C# projects I previously made:
- A .fnt viewer (files were extracted from .fon with BCC Workshop) 
- A PE resource reader to read DLL bitmaps.
- A raw bitmap reader.

The initial Peare with viewer capabilities opened Windows NE/PE formats, and was all wrote by hand.

I was slowly integrating other formats and resource support, understanding and learning formats, reading decades old manuals and PDFs.

Then the AI came. Most of the project was still done by hand but with AI support to understand formats structures.

In 2026 the AI development made a great step forward, so I decided to go all-in with it. I was able to refine the format structures faster but then I decided Peare had to get portable.

```System.Drawing```, ```System.Windows.Forms``` and ```DllImport``` were my biggest enemies at this point, so I made the AI port everything to QtWidgets. All what was supported in my handcrafted C# version has been ported to C++ with no compatibility loss found.

This is the current state of this project.

## Project goals

Peare aims to:

- open executable modules and application packages;
- expose their contents as navigable resource trees;
- preserve every original resource payload byte-for-byte;
- decode supported images, text resources, fonts, menus, dialogs, and related formats;
- keep the GUI detached from the backend;
- get one backend to list and expose the contents of a module or package (as an archiver);
- get one backend to read and parse to a more common format all the niche and historical formats inside;
- provide a stable C ABI without exposing Qt or C++ types, so can be used in other projects and languages with a simple wrapper;
- allow future plugins to extend containers, resource discovery, decoding, and presentation;
- expand beyond currently supported formats to other packages that contain resources worth inspecting.

Possible future package families include JAR, APK, APPX, XAP, MSI, and MSIX. These are project goals, not current compatibility claims.

## Target operating systems

The project is intended to remain portable and run natively on:

- [ ] OS/2;
- [ ] macOS;
- [X] Windows (modern);
- [ ] Windows (XP);
- [X] Linux;
- [X] Android.

## Current architecture

The current implementation is written in C++11 with Qt 5.15 and is divided into three products:

- _Peare_
- _PeareOpener_
- _PeareDecoder_

### Peare

The GUI opens files, builds the resource tree, displays decoded results, composes font glyph maps, exports original payloads and converted output, and provides hexadecimal fallback for unsupported resources.

### PeareOpener

The stateful Opener:

- opens PE, NE, LE, LX, XBE, XEX, XZP, LIVE, PIRS, and CON files or memory buffers;
- lists resource folders and entries;
- preserves numeric and textual identifiers, languages, codepages;
- reconstructs paged, segmented, continued, and large resources;
- returns the original payload and its source context.

It does not decode the resource contents.

### PeareDecoder

The Decoder accepts:

- a payload;
- an optional textual platform such as `Windows` or `OS/2`;
- an optional textual resource type such as `RT_BITMAP` or `RT_FONT`.

It returns simple C ABI outputs:

- arrays of complete PNG files;
- arrays of complete UTF-8 text files;
- typed metadata values;
- rendered font text as PNG.

When context is omitted, the Decoder examines the payload and uses RawDetect where useful.

Platform portability and packaging status is documented in `docs/PORTABILITY.md`.

It does not depend from PeareOpener in any way.

## Supported executable formats

The Opener currently handles:

| Platform | Format | Notes |
|---|---|---|
| Windows | NE | 16-bit |
| OS/2 | NE | 16-bit |
| Windows | PE | 32-bit and 64-bit |
| OS/2 | LE | 16-bit and 32-bit |
| OS/2 | LX | 32-bit |
| Xbox | XBE |  |
| Xbox 360 | XEX |  |

## Supported archives

The Opener additionally has support for uncommon archives:

| Format | Description | Notes |
|---|---|---|
| Xbox 360 XZP | Resource archive |  |
| OS/2 UnPack | IBM/Microsoft OS/2 PACK archive |  |
| Microsoft Compress | SZDD archive (variant A) |  |
| Siemens ProSave IMG |  Firmware image for HMIs |  |
| Siemens ProSave FWF |  Firmware image for HMIs |  |

## Resource compatibility

The matrix below preserves the compatibility declarations of the verified C# implementation on which the current port is based.

Legend:

- **Yes** — implemented;
- **No** — Not yet implemented;
- **N/A** — not known to exist
- **(1)** — known limitations or resources that may not decode correctly;
- **(2)** — implementation exists, but no suitable sample was available for verification;
- **(3)** — known incompatibility with Windows 1.x or 2.x resources.

| Resource | NE OS/2 | LX OS/2 | LE OS/2 | NE Windows | PE Windows |
|---|---:|---:|---:|---:|---:|
| `RT_POINTER` | Yes | Yes | Yes | N/A | N/A |
| `RT_MESSAGE` | Yes | Yes | Yes | N/A | N/A |
| `RT_MESSAGETABLE` | N/A | N/A | N/A | Yes (1)(3) | Yes |
| `RT_BITMAP` | Yes | Yes | Yes | Yes | Yes |
| `RT_STRING` | Yes | Yes | Yes | Yes (3) | Yes |
| `RT_DISPLAYINFO` | Yes | Yes | Yes | N/A | N/A |
| `RT_MENU` | Yes | Yes | Yes | Yes (3) | Yes |
| `RT_ACCELTABLE` | Yes | Yes | Yes | N/A | N/A |
| `RT_ACCELERATOR` | N/A | N/A | N/A | Yes | Yes |
| `RT_DLGINCLUDE` | Yes | Yes | Yes | — | — |
| `RT_HELPTABLE` | Yes (2) | Yes | Yes | — | — |
| `RT_HELPSUBTABLE` | Yes (2) | Yes | Yes | — | — |
| `RT_FONT` | Yes | Yes | Yes | Yes | Yes |
| `RT_FONTDIR` | Yes (1) | Yes (1) | Yes (1) | Yes (1) | Yes (1) |
| `RT_DIALOG` | Yes (1) | Yes (1) | Yes (1) | No | No |
| `RT_NAMETABLE` | — | — | — | Yes (3) | — |
| `RT_GROUP_ICON` | — | — | — | Yes | Yes |
| `RT_ICON` | — | — | — | Yes | Yes |
| `RT_VERSION` | — | — | — | Yes | Yes |
| `RT_GROUP_CURSOR` | — | — | — | Yes | Yes |
| `RT_CURSOR` | — | — | — | Yes | Yes |

OS/2 pointer and bitmap resources may contain `BA`, `BM`, `IC`, `CI`, `CP`, or `PT` structures and may produce multiple images.

The current C++ port continues to evolve. A format marked **Yes** can still contain undocumented or vendor-specific variants.

## Font compatibility

Font support as it is made now has yet to find a problematic sample that doesn't open correctly.

| Font format | Current declared support |
|---|---|
| Windows FNT version 1 | fixed-width raster, variable-width raster, vector |
| Windows FNT version 2 | fixed-width raster, variable-width raster |
| Windows FNT version 3 | fixed-width raster |
| OS/2 FNT | fixed-width raster, variable-width raster, GPI outline |

For a recognized font, the GUI obtains the character range and metrics through `peare_get_info()`, renders the sample sentence at 1x, 2x, 3x, and 4x through `peare_font_render()`, renders every character through the same function, and composes the glyph map locally.

## Raw payload detection

PeareDecoder can be used directly on a payload without first opening a PE, NE, LE, or LX container.

Examples include standalone OS/2 bitmap files and Windows or OS/2 FNT files. The optional platform and resource-type strings improve routing when the caller already knows the origin. When they are omitted, the Decoder examines the data structure and uses RawDetect as a fallback.

If no decoder succeeds, the caller still retains the original bytes. In the GUI, the application-level fallback is a hexadecimal dump.

## Public API documentation

The README intentionally does not duplicate individual function signatures. The complete ABI reference is kept in the dedicated documents so that declarations, ownership rules, status semantics, examples, and export lists remain in one place.

- [Common ABI types and memory ownership](docs/API_TYPES.md)
- [PeareOpener C API](docs/API_OPENER.md)
- [PeareDecoder C API](docs/API_DECODER.md)
- [Architecture and component responsibilities](docs/ARCHITECTURE.md)
- [Developer guide](docs/DEVELOPMENT.md)

The public headers are installed under `include/peare`.

## GUI usage

The basic workflow is:

1. open a supported executable module;
2. select a resource folder and entry from the tree;
3. inspect decoded images, text, metadata, font previews, or hexadecimal fallback;
4. export the original payload or converted PNG/TXT output.

The original payload is always obtained from the Opener and remains available even when decoding fails.

Detailed instructions are in [docs/GUI.md](docs/GUI.md).

## Building

Automated scripts for building are available for each plaform supported.

Manual build, install, and test procedures are documented in [docs/BUILDING.md](docs/BUILDING.md).

## Plugin intent

Plugin support is not implemented yet. It remains part of the project identity and long-term design.

A future plugin system is intended to support one or more of these roles:

1. **Extend containers and resource discovery**
   - add resources not exposed by the built-in Opener;
   - support additional content inside an existing module;
   - add entirely new package formats.

2. **Provide or alter resource listings**
   - expose additional folders or entries;
   - replace or intercept the default interpretation of a resource when necessary.

3. **Decode and present resources**
   - return images, text, or other neutral output;
   - optionally provide a GUI-specific presentation;
   - remain usable outside the GUI when no custom control is required.

Examples may be exposing .NET resources inside PE files and opening package formats such as JAR.

The plugin interface must not compromise byte-perfect extraction or the stability of the public C ABI.

## Project status

Peare is under active development and is not presented as production-stable software.

Historical executable formats contain undocumented structures, vendor variations, malformed resources, and platform-specific assumptions. Compatibility work therefore prioritizes:

- preservation of original bytes;
- explicit failures instead of fabricated output;
- testable ABI boundaries;
- gradual expansion based on real samples.

## Credits

**Third-party code**

| Project | Author |
| --- | --- |
| Qt | The Qt Company |
| wimlib (LZX compression) | Eric Biggers |
| cabextract (LZX code lineage, via wimlib) | Stuart Caie |
| Tiny AES in C | kokke |
| wmp-wsz-format (WMP skin WSZ layout) | Ted de Baets (tdebaets) |

**Documentation and format references**

| Source | Where |
| --- | --- |
| wimlib | https://wimlib.net/ · https://github.com/ebiggers/wimlib |
| Tiny AES in C | https://github.com/kokke/tiny-AES-c |
| WMP WSZ format | https://github.com/tdebaets/wmp-wsz-format |
| IBM OS/2 16/32-bit Object Module Format | https://www.edm2.com/index.php/IBM_OS/2_16/32-bit_Object_Module_Format_ |
| Resources and Decompiling Them | https://www.edm2.com/index.php/Resources_and_Decompiling_Them |
| IBM OS/2 2.0 Technical Library (Presentation Driver Reference) | https://bitsavers.trailing-edge.com/pdf/ibm/pc/os2/ |
| Windows 1.03 / 2.0 SDK Programmer's Reference | https://www.os2museum.com/ |
| BetaArchive format discussion | https://www.betaarchive.com/forum/viewtopic.php?t=33486 |

## Copyright

All format specifications, product names, and trademarks referenced or supported
by Peare are the property of their respective owners — including, among others,
Microsoft Corporation, IBM Corporation, and Siemens AG. Peare provides
independent, clean-room readers and is not affiliated with, authorized by, or
endorsed by these companies.

## Documentation index

- [GUI usage](docs/GUI.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Building, installation, and tests](docs/BUILDING.md)
- [Common ABI types](docs/API_TYPES.md)
- [PeareOpener API](docs/API_OPENER.md)
- [PeareDecoder API](docs/API_DECODER.md)
- [Developer guide](docs/DEVELOPMENT.md)
