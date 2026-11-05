# Architecture

Peare has a GUI and three libraries. Each library owns one part of the work.

```text
Peare GUI
├── PeareOpener
├── PeareDecoder
└── PeareWriter
```

## Components

### PeareOpener

The Opener opens files and byte buffers. It detects containers and keeps an
open session. It lists folders and resources. It returns the original payload
and its context. It does not decode payloads.

The Opener also handles whole-module conversions. For example, it can rebuild
a PE file layout from a loaded image.

### PeareDecoder

The Decoder receives a payload and optional platform and resource-type strings.
It returns PNG images, UTF-8 text, typed values, or rendered font images.
It does not open containers.

### PeareWriter

The Writer creates and updates supported containers through a generic C ABI.
The GUI asks the Writer which operations it supports. The Writer selects the
backend and performs each operation.

The current write variants are listed in [the format matrix](../SUPPORTED_FORMATS.md).
The [Writer ABI guide](../ABI/API_WRITER.md) defines each operation and its limits.

### Peare GUI

The GUI opens containers through the Opener. It passes payloads and context to
the Decoder. It sends generic file operations to the Writer. It displays
decoded output and exports original or converted data.

For a standalone payload, the GUI passes the file bytes directly to the Decoder.
The Opener does not detect or parse font formats. It returns container records
as raw bytes. The Decoder identifies and interprets those bytes, whether they
come from a standalone file or a container resource.

## Data flow

```text
file or byte buffer
        ↓
PeareOpener
        ↓ payload and context
Peare GUI
        ↓ payload and context strings
PeareDecoder
        ↓ PNG, UTF-8 text, values, or font images
Peare GUI
```

The GUI converts platform identifiers to text such as `Windows`, `OS/2`, or
`Other`. It passes resource types such as `RT_BITMAP` as text.

The Opener sends each eligible nested payload through the same module factory.
This lets supported containers open at any nesting level. A module exposes its
own payloads. It does not open child containers itself.

## Decoder routing

The Decoder uses the caller's resource type first. It then uses the platform,
payload structure, RawDetect, and a final heuristic route.

The caller can pass `NULL` or an empty string when context is not available.
The Decoder then tries to detect the payload format.

Group icon and cursor resources contain descriptors and child identifiers.
They do not contain the child image pixels. The GUI asks the Opener for each
child resource, then sends each payload to the Decoder.

## Fonts

The Decoder renders text from a font as a PNG image. The GUI reads the font's
font mappings and metrics through the Decoder ABI. It renders sample text at
scales 1x to 4x. It renders glyphs by index when the font supports that operation.

## ABI boundaries

Public headers expose C types only. They do not expose `QString`, `QByteArray`,
`QImage`, STL types, C++ classes, or C++ exceptions.

Each library exports its declared C symbols. Platform visibility macros control
symbol export. Public headers live with the library that owns the ABI.

The Writer does not depend on the Opener. The GUI coordinates both libraries.
The Decoder also does not depend on the Opener.

## Executable resources

The XEX module parses XEX1 and XEX2 containers. It sends the embedded PE image
to the PE resource parser. The current XEX path accepts directly readable PE
images. See the format matrix for encryption and compression limits.

The XBE module validates the `XBEH` header. It reads the certificate, UTF-16
title, sections, TLS data, library versions, and kernel import tables. It
preserves the original logo payload. The Decoder does not yet decode its RLE
stream.

The XBE module scans only validated sections for XPR packages and DDS headers.
It rejects malformed XPR sizes and headers. It exposes the original XBE header
signature and section SHA-1 values. It does not claim RSA authenticity.

## Resource tree

The Opener does not add artificial roots such as `PE structure`, `Sections`, or
`Resources`. Native structures appear directly in the tree.

PE sections appear under the module. Resources appear under the `.rsrc` section.
NE modules expose loadable code and data areas. LE and LX modules use object
flags to identify `.text`, `.data`, and `.bss` areas. Resource objects appear
under one `.rsrc` root. Headers remain selectable payloads.

XBE payloads remain under their named sections. The same tree rules apply to
standalone modules and modules inside another container.

WAD lumps remain in one flat list and keep their directory order. Namespace
ranges classify graphics, and map markers identify the following map lumps.
Each lump remains selectable and retains its original name and payload.

## Xbox STFS packages

LIVE, PIRS, and CON packages belong to the Opener. Their modules expose package
metadata, icons, directories, and files. Shared STFS code lives in
`src/opener/modules/stfs`.

All resource decoders, including FMIM and Windows or OS/2 resource types, live
under `src/decoder/resources`.
