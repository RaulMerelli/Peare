# Adding an Opener module

This guide is for maintainers who change Peare. Follow it as a checklist.

Two rules govern every module:

1. **A module parses only its own container.** It exposes the embedded byte
   payloads and nothing else. It never instantiates or calls another module, and
   it never pre-classifies a payload as a foreign format.
2. **Nesting uses one path.** `OpenerSession` opens nested resources through
   `ModuleFactory` when the user requests them. The public entry point is
   `peare_opener_open`.

Choose exactly one content representation for each resource. Use a flat array
(`ResourceEntry::data`) or a lazy, layer-backed store (`ResourceEntry::content`).
All formats use the same detection, dispatch, nesting, and C ABI paths.
Do not add other format-specific behavior.

---

## Two module archetypes

Pick the one that matches your format.

### A. Content module (executables, archives, firmware)

The module reads the whole container and produces one `ResourceEntry` per
contained item (an OS/2 PACK member, an XEX section, a reconstructed PE, …).
Examples: `PeFileModule`, `NeModule`, `Os2PackModule`, `XexModule`, `SzddModule`.

### B. Filesystem module (disc/volume images)

The module is a thin bridge over a reader built on Peare's **DiscUtils-compatible
filesystem stack** (`src/opener/fs`). Some readers port or adapt DiscUtils code;
see [third-party notices](../../THIRDPARTY.md). The reader parses the format;
the module enumerates files as resources. Examples: `IsoModule`, `WimModule`,
`FatModule`, and `UdfModule`.

The file-system stack uses Qt-free, endian-safe C++11 code:

- `fs/DiscStore.h` — `IByteStore` provides positioned random-access reads.
  It follows the DiscUtils `IBuffer` pattern. Implementations include
  `MemoryStore`, `SubStore`, `ConcatStore`, `ZeroStore`, and `ExternalStore`.
  These stores expose file data on demand without copying it.
- `fs/DiscFileSystem.h` — `IDiscFileSystem`: `friendlyName()`, `valid()`,
  `error()`, `list(dir)` and `openFile(path) -> ByteStorePtr`.
- `fs/BlockDecompressor.h` — `IBlockDecompressor` and the ported decompressors
  (LZX via `peare_lzx_wim_*`, XPRESS Huffman) for chunked/compressed content.

To add a file system, implement `IDiscFileSystem` in
`fs/<Fs>Reader.{h,cpp}`. Use a `ByteStorePtr` for the input.
Then add the bridge module shown in archetype B.

**List one directory at a time.** A file-system module does not walk the full
tree. A full OS image can take minutes to scan.
`open(disc, name, subPath)` lists only `subPath`. The default path is the root.
Call the shared `modules/FsLevel.h buildFsLevel()` function:

- A file becomes a `<FS>_FILE` leaf. Set lazy `content` and
  `isEmbeddedFile = true`.
- A subdirectory becomes a `<FS>_DIR` container entry. Set
  `ResourceEntry::isDirectory = true`, `containerSubPath` to the child path,
  and `content` to the whole-image store.

When the user opens a directory, the Opener reopens the image at
`containerSubPath`. It uses `peare_resource_get_source()` and
`peare_opener_open()`.
`OpenerSession::openStore(store, name, subPath)` routes to
`ModuleFactory::open(disc, name, subPath)`. The factory calls
`<Fs>Module::open(disc, name, subPath)`.
This path opens both nested files and directories. Call `buildFsLevel()`.
Do not add a recursive `walk()`.

### C. Virtual-disk container (VMDK, VHD, VDI, QCOW, VHDX, and later raw)

A virtual disk exposes a raw disk, not a file system. MBR or GPT points to
partitions. Each partition can contain a file system.
The module creates a logical-disk `ByteStore`. For example, `fs/VmdkDisk`
resolves VMDK grain tables.
Pass the store to `fs/PartitionTable`. Create one `DISK_PARTITION` resource for
each partition. Set its `content` to a `SubStore` window on the disk.
Set `isEmbeddedFile` on each partition. The Opener detects the file system and
opens it through the nested path.
This supports paths such as VMDK to partition to exFAT to file.
Reuse `PartitionTable` for new disk formats. Only the raw-disk reader is
format-specific.

---

## 1. Define and detect the format

Add the format to `ModuleFormat` in `src/opener/modules/ModuleFormat.h`, then
update **both** detectors and the name function in `ModuleFormat.cpp`:

- `ModuleFormatDetector::detectFile(const QString&)` — detects a top-level file.
  It can search for a signature at a known offset. ISO 9660 uses the `CD001`
  probe. UDF scans the volume-recognition sequence before it reads the file head.
- `ModuleFormatDetector::detectBuffer(const QByteArray&)` — **header-only**
  detection over an in-memory buffer. This is used for the nested peek, so it must
  recognise the format from a prefix. `ModuleFactory::open(ByteStorePtr)` passes
  the first bytes here; enlarge that peek if your magic sits past the current
  window (UDF pushed it to `0x14000`).
- `ModuleFormatDetector::formatName(ModuleFormat)`.

Detection must use file contents only — never extensions or resource names. The
same detectors serve top-level files and embedded payloads. Order matters: put a
more specific magic before a looser one (UDF is checked before ISO 9660 so bridge
discs open as UDF).

## 2. Implement the module

Create `src/opener/modules/<Format>Module.{h,cpp}` implementing `IModule`, and
`IResourceContainer` when the format contains files/resources.

Provide two static entry points so the module works both top-level and nested
without materialising the whole image:

```cpp
static ModulePtr open(const QString& filePath);                       // top-level
static ModulePtr open(const fs::ByteStorePtr& disc, const QString& name); // nested
```

The path form memory-maps the file into an `ExternalStore` (fallback:
`MemoryStore`) and forwards to the store form. The store form does the real work.
This is the pattern in every filesystem module; content modules can keep a single
byte-array entry point when they have no lazy backing.

**A module must not instantiate another module.** Do not call `PeFileModule`,
`XuizModule`, another `<Format>Module`, or `ModuleFactory` from a module.

## 3. Expose resources correctly

For each contained file, push one `ResourceEntry`:

- `name` — the contained logical filename or a stable identifier.
- `type` — a type string owned by *this* container, e.g. `OS2_PACK_FILE`,
  `WIM_FILE`, `FAT_FILE`, `UDF_FILE`.
- `dataSize` — the payload size.
- `hierarchyPath` — the path **inside this container only** (directory segments,
  leaf excluded).
- Content, choosing one representation:
  - **Lazy (preferred for images):** set `content` to the `ByteStorePtr` from
    `IDiscFileSystem::openFile(...)`. It is read on demand; nothing is copied at
    enumeration time.
  - **Array:** set `data` to the complete payload bytes.
- `isEmbeddedFile = true` — this marks the entry as a whole embedded file, i.e. a
  candidate for nested opening. Structural sub-resources (RT_* icons, PE sections,
  headers) **must leave it false** so they are never probed.

Do **not** set `format` to a foreign type (PE/NE/…). Leave it
`ModuleFormat::Unknown`; the session's peek fills in the recognised format for
container entries. Only set `format` when it describes *your* module's own
structural resource.

Do **not** append a child module's resources yourself, and do not synthesise a
whole-container navigation root. Synthetic structural entries (headers, tables,
areas) must use clearly structural `type` strings so the session excludes them
from nested detection and folder flattening.

## 4. Register the module once

- **CMakeLists.txt** — add the reader (`fs/<Fs>Reader.{h,cpp}`, filesystem only)
  and the module (`modules/<Format>Module.{h,cpp}`) to the `PeareOpener` sources.
- **`ModuleFactory.cpp`** — `#include` the module and add one dispatch branch in
  **each** relevant factory:
  - `open(const QString&)` — top-level path dispatch.
  - `open(const fs::ByteStorePtr&, const QString&)` — nested dispatch, for
    filesystem formats that can be opened lazily from a store. Content-only
    formats fall through to the materialise-then-`open(QByteArray)` path.

`ModuleFactory` is the only format-to-module dispatcher. Do not add parallel
dispatch tables or nested-format special cases anywhere else.

## 5. Wire the C ABI and folder model

- **`src/opener/api/peare/peare_types.h`** — add `PEARE_CONTAINER_<FORMAT>` to
  `peare_container_format`.
- **`src/opener/api/Api.cpp`** — map it in `toCFormat()`.
- **`src/app/PeareApi.cpp`** — map it to a display name in the container-name
  switch.
- **`src/opener/OpenerSession.cpp`** — add a selectable `*_FILE` type to the
  `selectableContainer` list in `rebuildFolders()`. This keeps the file as a
  selectable leaf instead of wrapping it in another type folder.
  `adoptModule()` already marks directory entries as containers.
  Modules that use `buildFsLevel` need no directory changes here.
  Add a `platformFor()` case only when the format identifies a platform.

## 6. How nesting works (do not reimplement)

`OpenerSession::adoptModule()` copies the module resources.
For each `isEmbeddedFile` entry, it reads the first 4 KiB with
`content->readRange` or `data.left`.
It passes those bytes to `detectBuffer`.
When the detector finds a format, the session sets `isContainer = true` and
records the format. It does not expand the resource at this stage.

When the user opens a nested resource, the GUI calls
`peare_resource_get_source()`. The function returns a `peare_source_handle` over
the resource store or its data.
The GUI passes the handle to `peare_opener_open()`.
The Opener routes it to `ModuleFactory::open(ByteStorePtr)` and the nested
format branch. This one path handles PE in ISO, WIM in ISO, and files in UDF.
Do not add nesting code to a module.

## 7. Verify

Build with the platform script (e.g. `tools/build/build_windows.bat`, invoked with null
stdin) and confirm C++11 portability for the readers:

```bash
g++ -std=c++11 -Wall -Wextra -fsyntax-only src/opener/fs/<Fs>Reader.cpp -Isrc/opener
```

Test:

1. top-level detection and opening;
2. malformed / truncated input yields an explicit error, not a crash or fabricated
   output;
3. payload extraction is byte-exact (compare an extracted file against the source;
   verify format framing, e.g. a JPEG ends `FFD9`);
4. the new format nested inside an existing container (and vice-versa) opens
   through the single path without materialising the whole image;
5. no duplicate `name -> name` nodes and no synthetic top-level root;
6. recursion/loop and depth protection (bounded chain/extent walks).

## Review checklist

- The module parses only its own format; it opens no other module.
- Content is exposed once, either as `content` (lazy) or `data` (array) — never
  both, and no third representation is introduced.
- `isEmbeddedFile` is set only on whole embedded files; structural entries leave
  it false.
- Detection is signature-based and present in **both** `detectFile` and
  `detectBuffer`; `formatName` updated.
- Dispatch is added in `ModuleFactory` (path, and store form for filesystems);
  no dispatch lives elsewhere.
- The C ABI is complete: `PEARE_CONTAINER_*`, `toCFormat`, container display name,
  and the `selectableContainer` list where applicable.
- Readers compile clean under `-std=c++11` and read integers byte-wise (no
  multi-byte `reinterpret_cast`), so they are endian-safe on all declared targets.
- Existing nested combinations still open.
