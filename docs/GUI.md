# GUI usage

## Opening a file

Open a supported executable or module. The Opener recognizes DOS MZ, PE, NE, LE, LX, XBE, XEX1/XEX2, XUIZ/XZP, LIVE, PIRS, and CON containers and builds the tree:

```text
file
└── resource type/folder
    └── resource
```

Each resource preserves its identifier, language, codepage when known, offset, size, and platform.

## Preview

When a resource is selected, the GUI requests from the Decoder:

- PNG through `peare_decode_images()`;
- UTF-8 TXT through `peare_decode_texts()`;
- metadata through `peare_get_info()`;
- font rendering through `peare_font_render()`.

When no decoder produces a valid result, the GUI displays a hexadecimal dump of the original payload. The hex dump is a GUI fallback, not Decoder output.

## Images

Images are displayed as PNG. A resource may produce multiple images, for example:

- bitmap array OS/2;
- group icon/cursor;
- pointer arrays.

## Text

Menus, dialogs, string tables, message tables, and other textual representations are displayed as UTF-8.

## Font

For a recognized font, the GUI displays:

- textual information;
- a sample sentence at 1x, 2x, 3x, and 4x;
- a glyph map composed by the GUI.

The glyph map is created by querying the character range and rendering one character at a time with the same API used for the sample sentence.

## Group icon and group cursor

The group resource is displayed textually. The GUI also resolves child-image identifiers through the Opener and displays the PNG files produced by the Decoder.

## Exports

### Original

Exports the byte-perfect payload returned by the Opener. The extension is selected from the type or payload signature, for example `.bmp`, `.ico`, `.cur`, `.fnt`, or `.bin`.

### Converted

Exports PNG or TXT files produced by the Decoder. When multiple items exist, the GUI creates numbered files in a directory selected by the user.

### All resources

Exports all original payloads in a directory structure organized by resource type.


## Open-file filters

The file dialog lists all currently supported module families: DOS/Windows/OS/2 executables, PE, NE, LE/LX, Xbox executables, and Xbox 360 archives/packages. These filters only help navigation. Peare always identifies the selected file from its binary signature, so extensionless embedded modules and files with unusual extensions remain supported through **All files**.

## Native application menu

Peare creates its actions before its menus, inserts them through `QMenuBar::addMenu()` / `QMenu::addAction()`, and explicitly keeps the `QMenuBar` native. This mirrors Qt's Main Windows Menus example and allows platform integrations such as Android's options/overflow menu to receive the complete action hierarchy at startup. The same code path is used on every operating system.
