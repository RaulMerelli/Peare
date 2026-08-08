# Adding an Opener module

This guide is written for AI agents and maintainers modifying Peare. Follow it as a checklist.

Two rules govern every module:

1. **A module parses only its own container.** It exposes the embedded byte
   payloads and nothing else. It never instantiates or calls another module, and
   it never pre-classifies a payload as a foreign format.
2. **Nesting is centralised.** Opening a resource that is itself a container is
   done exclusively by `OpenerSession` through `ModuleFactory`, on demand, behind
   the single public entry point `peare_opener_open`.

There is exactly **one permitted representation choice** per resource: its content
bytes may be a flat array (`ResourceEntry::data`) *or* a lazy, layer-backed store
(`ResourceEntry::content`). Everything else — detection, dispatch, nesting, the C
ABI — is uniform across all formats. Do not introduce any other per-format
divergence.

---

## Two module archetypes

Pick the one that matches your format.

### A. Content module (executables, archives, firmware)

The module reads the whole container and produces one `ResourceEntry` per
contained item (an OS/2 PACK member, an XEX section, a reconstructed PE, …).
Examples: `PeFileModule`, `NeModule`, `Os2PackModule`, `XexModule`, `SzddModule`.

### B. Filesystem module (disc/volume images)

The module is a thin bridge over a reader built on the **DiscUtils-compatible
filesystem stack** (`src/opener/fs`). The reader does the parsing; the module just
enumerates files as resources. Examples: `IsoModule`, `WimModule`, `FatModule`,
`UdfModule`.

The filesystem stack (all Qt-free, C++11, endian-safe) is:

- `fs/DiscStore.h` — `IByteStore` (positioned random-access read == DiscUtils
  `IBuffer`) and its implementations: `MemoryStore`, `SubStore` (a window),
  `ConcatStore` (concatenation), `ZeroStore` (implicit zeros), `ExternalStore`
  (references mmap'd memory and keeps the owner alive). These are the layers that
  expose file content lazily without copying.
- `fs/DiscFileSystem.h` — `IDiscFileSystem`: `friendlyName()`, `valid()`,
  `error()`, `list(dir)` and `openFile(path) -> ByteStorePtr`.
- `fs/BlockDecompressor.h` — `IBlockDecompressor` and the ported decompressors
  (LZX via `peare_lzx_wim_*`, XPRESS Huffman) for chunked/compressed content.

To add a filesystem, port the DiscUtils reader into a `fs/<Fs>Reader.{h,cpp}`
implementing `IDiscFileSystem` over a `ByteStorePtr`, then write the bridge module
(archetype B pattern below).

**Enumeration is lazy, one directory at a time.** A filesystem module never walks
the whole tree — that made a full-OS image (a 428 GB ext4 root) take minutes. Its
`open(disc, name, subPath)` lists only the directory `subPath` (default root) by
calling the shared `modules/FsLevel.h buildFsLevel()`:

- a file becomes a `<FS>_FILE` leaf with lazy `content` and `isEmbeddedFile=true`;
- a subdirectory becomes a `<FS>_DIR` container entry (`ResourceEntry::isDirectory
  = true`, `containerSubPath` = the child path, `content` = the whole-fs image
  store).

Navigating a directory reopens the fs image at `containerSubPath` through the
**same** `get_source` + `peare_opener_open` path the nested-container flow uses:
the source carries the subpath, `OpenerSession::openStore(store, name, subPath)`
routes to `ModuleFactory::open(disc, name, subPath)` → `<Fs>Module::open(disc,
name, subPath)`. So there is one navigation mechanism for both "a file that is a
container" and "a directory". You get all of this for free by calling
`buildFsLevel` — do not re-add a recursive `walk()`.

### C. Virtual-disk container (VMDK, VHD, VDI, QCOW, VHDX, and later raw)

A virtual disk is not a file system: it exposes a raw disk, inside which a
partition table (MBR/GPT) points at partitions, each holding a file system. The
module produces the logical-disk `ByteStore` (e.g. `fs/VmdkDisk` resolves VMDK
grain tables), runs the shared `fs/PartitionTable` reader over it, and emits one
`DISK_PARTITION` resource per partition whose `content` is a `SubStore` window over
the disk. Because each partition is `isEmbeddedFile`, the session's peek detects
the file system inside and opens it through the same nested path — so `VMDK/VHD/VDI/QCOW/VHDX ->
partition -> exFAT -> file` works with no new nesting code. Reuse `PartitionTable`
for any future disk container; only the raw-disk producer is format-specific.

---

## 1. Define and detect the format

Add the format to `ModuleFormat` in `src/opener/modules/ModuleFormat.h`, then
update **both** detectors and the name function in `ModuleFormat.cpp`:

- `ModuleFormatDetector::detectFile(const QString&)` — top-level detection from a
  file path. It may seek cheaply for a magic deep in the file (see the ISO 9660
  `CD001` probe and the UDF volume-recognition-sequence scan) before falling back
  to reading the head.
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
- **`src/opener/OpenerSession.cpp`** — if your module emits a selectable
  `*_FILE` container type, add it to the `selectableContainer` list in
  `rebuildFolders()` so the file appears as a selectable leaf (not wrapped in an
  extra type folder). Directory entries (`entry.isDirectory`) are already covered
  there and marked as containers in `adoptModule`, so `buildFsLevel`-based modules
  need no change here. Add a `platformFor()` case only if the format implies a
  platform.

## 6. How nesting works (do not reimplement)

`OpenerSession::adoptModule()` copies the module's own resources and, for each
`isEmbeddedFile` entry, peeks the first 4 KiB (via `content->readRange` or
`data.left`) and runs `detectBuffer`. If a known format is recognised it sets
`isContainer = true` and records the recognised `format`. Nothing is expanded
eagerly.

When the consumer navigates into such a resource, the GUI/ABI calls
`peare_resource_get_source` (→ a `peare_source_handle` over the resource's
`content` store, or its `data`) and then `peare_opener_open` on it. That routes to
`ModuleFactory::open(ByteStorePtr)` → your nested branch. This single path serves
every combination (PE-in-ISO, WIM-in-ISO, ESP-FAT-in-image, file-in-UDF, …). You
never write nesting code in a module.

## 7. Verify

Build with the platform script (e.g. `build_windows.bat`, invoked with null
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
