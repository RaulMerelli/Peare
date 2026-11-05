# Project origin

## The name

Peare means **Portable Editor of Archives, Resources, and Executables**.
The name describes the project's first purpose: read/write files of every kind

## Project history

Peare began as the merge of three C# projects:

- A viewer for Windows FNT files extracted from FON files with BCC Workshop.
- A PE resource reader for bitmaps in DLL files.
- A reader for raw bitmap files.

The first Peare viewer opened Windows NE and PE files. The maintainer wrote
that version by hand.

The project then gained more formats and resources. The maintainer studied old
manuals and PDF documents. AI tools later helped explain format structures.

In 2026, the maintainer chose an AI-assisted development process and moved the
application to Qt Widgets. The port replaced `System.Drawing`,
`System.Windows.Forms`, and `DllImport`. The maintainer reports no known loss
of compatibility with the C# version.

## Project direction

Peare uses a small C ABI between the GUI and its backends. The backends contain
the format logic. New formats are added to the core instead of an external
plugin system.

The project aims to preserve source data, expose nested resources, and add
format support in small, testable steps. See [the format matrix](../SUPPORTED_FORMATS.md)
for current read and write support. See [the roadmap](ROADMAP.md) for planned work.
