# GUI usage

## Opening files and folders

Use **File > Open file** to open a local file.
Peare detects containers from their contents, not from the file extension.
You can preview unsupported files when Qt can read them as images.

Drop files onto an open local folder to copy them there. Drop files while a
container with Writer support is open to add them to that container.
Peare does not overwrite existing files during a folder drop.
In other contexts, dropping one file opens it or shows its image preview.

Use **File > Open folder** to browse a local directory in the same tree and Details views. Directories are loaded as they are expanded. Double-click a file to open it as a Peare module or image preview.

For supported containers, the Opener builds the tree:

```text
file
└── resource type/folder
    └── resource
```

Each resource preserves its identifier, language, codepage when known, offset, size, and platform.

## Preview

When you select a resource, the GUI asks the Decoder for these items.
The preview limit is 16 MiB.

- PNG through `peare_decode_images()`.
- UTF-8 TXT through `peare_decode_texts()`.
- Metadata through `peare_get_info()`.
- Font rendering through `peare_font_render()`.

When no decoder produces a valid result, the GUI displays a hexadecimal dump of the original payload up to 1 MiB. Larger payloads show a warning instead of allocating a large hex view. Payloads over 16 MiB are not decoded for preview and show a size warning. These limits do not prevent exporting the original resource.

## Images

Images are displayed as PNG. Standalone image files can also be previewed without being recognized as containers. A resource may produce multiple images, for example:

- OS/2 bitmap arrays.
- Group icons or cursors.
- Pointer arrays.

## Text

Menus, dialogs, string tables, message tables, and other textual representations are displayed as UTF-8.

## Font

For a recognized font, the GUI displays:

- Text information.
- A sample sentence at 1x, 2x, 3x, and 4x.
- A glyph map built by the GUI.

The GUI gets font mappings and glyph counts through the Decoder ABI.
It renders one cell for each glyph when the font supports glyph-index rendering.
This includes glyphs that do not have a Unicode mapping.

## Group icon and group cursor

The group resource is displayed textually. The GUI also resolves child-image identifiers through the Opener and displays the PNG files produced by the Decoder.

## Exports

### Original

Exports the original payload from the Opener.
Peare keeps an extension from the resource identifier when one exists.
Otherwise, it uses the resource type or payload signature to select an extension.
The context menu uses the same rule.

### Converted

Exports PNG or TXT files produced by the Decoder. When multiple items exist, the GUI creates numbered files in a directory selected by the user.

### All leaves

Exports leaf resources under the currently selected tree item or Details location, including resources inside nested containers that have not yet been opened. It recreates the visible resource hierarchy and preserves available filename extensions.

## Editing open containers

Use **File > Add files to...** to select files, then choose a format, variant,
filesystem when the format is a virtual disk, and destination. PeareWriter
provides these choices.
Use **Resource > Add files to...** to add files to the open container when its
Writer backend supports the operation.
New archive entries use their source filenames.
Drop files on a folder to add them there. Drop files on an entry to add them to
that entry's parent folder. In local folder mode, the same rule copies files.

Use **Edit > New folder** or **Edit > New file** to create entries when the
Writer reports those operations. Select an entry and use **Rename**, **Delete**,
or **Replace file** when the Writer reports those operations. The same commands
are available in the context menu. ZIP, TAR, and supported FAT targets
expose these operations.
The GUI reads operation support from the Writer ABI. Disk images with multiple
FAT volumes prompt for the target volume before an update.

ZIP updates keep existing Store, Shrink, Deflate, and ZIP64 metadata.
TAR updates keep existing entries and support create, replace, rename, and
delete operations for USTAR entries.
FAT12, FAT16, and FAT32 images support file and directory creation, replacement,
rename, and deletion with UTF-8 long names. FAT writes use a staged copy of the
full image. The generic add-files command can also create standalone FAT12,
FAT16, and FAT32 images. It can create fixed or dynamic VHD and VHDX disks, and
monolithic sparse VMDK disks. Select FAT12, FAT16, or FAT32 for each new disk.

Supported FAT targets include standalone images and single-FAT-partition MBR
disks. They also include fixed VHD, standalone dynamic VHD Type 3, and fixed or
dynamic VHDX.
Supported VMDK targets include standalone monolithicSparse and monolithicFlat.
They also include standalone streamOptimized and split twoGbMaxExtentSparse
and twoGbMaxExtentFlat disks. Sparse updates require valid redundant grain
metadata.
The Writer converts a streamOptimized disk to monolithicSparse during an update.

The Writer does not support parented disks, VMFS or device-backed extents, or
compressed VMDK layouts other than streamOptimized. It does not support VHD or
VHDX differencing disks, extended MBR partitions, VDI, or QCOW. ZIP members keep
the Opener's 512 MiB uncompressed-size limit.
Multi-disk ZIP and unsupported TAR variants are not supported.

## Local folder operations

When you browse a local folder, **Edit** and the context menu can create folders,
empty files, symbolic links, and Windows junctions. They can also rename and
delete items. These actions change the file system at once.
They are available only in folder mode. They do not change open archives.
Deleting a directory also deletes its contents.
Deleting a symbolic link or junction does not follow its target.

## Layout

Use **View > Navigation pane** and **View > Preview pane** to show or hide either side of the splitter. Drag the divider to resize the panes.

## File associations

Use **File associations** on the toolbar to manage extensions by category.
On Linux, selected types become per-user defaults. Clearing a type restores its
previous default.

On Windows, Peare registers the selected types in Windows Default Apps and opens
that system page. Select Peare there to change the defaults. Peare does not edit
the Open with lists.

Android controls file handlers through system settings. Android does not let an
app assign itself as the default handler. Other platforms need native support.

## Native application menu

Peare creates its actions before its menus, inserts them through `QMenuBar::addMenu()` / `QMenu::addAction()`, and explicitly keeps the `QMenuBar` native. This mirrors Qt's Main Windows Menus example and allows platform integrations such as Android's options/overflow menu to receive the complete action hierarchy at startup. The same code path is used on every operating system.
