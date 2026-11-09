# PeareWriter C API

Header:

```c
#include <peare/peare_writer.h>
```

PeareWriter creates and updates containers. Its C ABI does not depend on
PeareOpener. The GUI reads format and variant IDs from Writer descriptors and
passes the selected IDs to generic operations. It does not branch on specific
format IDs.

## Generic creation

The caller discovers available formats and variants through the Writer. The
batch `peare_writer_add_files_to_ex()` call creates a new container and adds
all selected files, or updates an existing target and adds all selected files. A
failed batch is aborted. The older begin/add/finish functions remain available
for streaming clients.

```c
size_t peare_writer_create_format_count(void);
peare_writer_status peare_writer_get_create_format(
    size_t index, peare_writer_format *out_format);
size_t peare_writer_create_variant_count(const char *format_id_utf8);
peare_writer_status peare_writer_get_create_variant(
    const char *format_id_utf8, size_t index,
    peare_writer_variant *out_variant);
peare_writer_status peare_writer_create_begin(
    peare_writer_handle writer, const char *format_id_utf8,
    const char *output_path_utf8);
peare_writer_status peare_writer_create_begin_variant(
    peare_writer_handle writer, const char *format_id_utf8,
    const char *variant_id_utf8, const char *output_path_utf8);
peare_writer_status peare_writer_add_files_to(
    peare_writer_handle writer, const char *target_path_utf8,
    const char *format_id_utf8, const char *variant_id_utf8,
    size_t target_index, const peare_writer_input_file *files,
    size_t file_count);
size_t peare_writer_create_filesystem_count(const char *format_id_utf8);
peare_writer_status peare_writer_get_create_filesystem(
    const char *format_id_utf8, size_t index,
    peare_writer_variant *out_filesystem);
peare_writer_status peare_writer_add_files_to_ex(
    peare_writer_handle writer, const char *target_path_utf8,
    const peare_writer_create_options *create_options, size_t target_index,
    const peare_writer_input_file *files, size_t file_count);
peare_writer_status peare_writer_add_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
peare_writer_status peare_writer_finish(peare_writer_handle writer);
```

The create backends are ZIP, GZIP, BZIP2, Unix compress, LZMA-alone, XZ,
Zstandard, TAR, TAR.GZ, TAR.BZ2, TAR.ZST, TAR.Z, TAR.XZ, TAR.LZMA, GNU and BSD ar, Doom IWAD and PWAD,
all six CPIO variants (`newc`, CRC, `odc`, afio
large ASCII, binary little-endian, and binary big-endian), standalone FAT, floppy images,
raw MBR/GPT disks, VHD, VHDX, and VMDK. Format descriptors and their strings remain valid
for the process lifetime. Floppy images support the 12 geometries in the Writer
variant list. Raw disks support MBR and GPT layouts. VHD and VHDX support fixed
and dynamic variants. VMDK supports monolithic sparse and split sparse creation.
Split creation uses one external extent with a 2 GiB maximum. VHD, VHDX, VMDK,
and raw MBR/GPT creation support FAT12, FAT16, FAT32, exFAT, and NTFS. NTFS
volumes are formatted by the bundled NTFS-3G formatter on the staged image and
are checked before Peare publishes the new disk. Floppy images use FAT12.

GZIP creation writes one RFC 1952 member per selected input file. The GZIP
stream is not an archive with independently editable entries.

TAR.GZ creation writes a USTAR archive in one RFC 1952 GZIP member. The Writer
can also update supported TAR.GZ and TGZ files. It validates each input member's
header, Deflate stream, CRC32, and uncompressed size before it accepts the
decompressed TAR data. Updates write one GZIP member and USTAR headers.

TAR.BZ2 creation writes a USTAR archive in one BZIP2 stream. The Writer can also
update supported TAR.BZ2, TBZ, and TBZ2 files. It validates each BZIP2 stream
before it accepts the decompressed TAR data. Updates write one BZIP2 stream and
USTAR headers. The Writer supports compression levels 1 to 9.

TAR.ZST creation writes a USTAR archive in one Zstandard frame. The Writer can
also update TAR.ZST, TZST, and TAR.ZSTD files. It validates each Zstandard frame
before it accepts the decompressed TAR data. Updates write one Zstandard frame
and USTAR headers. The Writer supports compression levels -5 through 22.

TAR.Z creation writes a USTAR archive in one Unix compress stream. The Writer
can also update TAR.Z and TAZ files. It validates the compressed stream and the
decompressed TAR before an update. The Writer supports block and non-block LZW
with code widths from 9 through 16 bits.

TAR.XZ creation writes a USTAR archive in one XZ stream. The Writer can also
update TAR.XZ and TXZ files. It validates the XZ stream and the decompressed TAR
before an update. The Writer supports no check, CRC32, CRC64, and SHA-256 checks.

TAR.LZMA creation writes a USTAR archive in one LZMA-alone stream. The Writer
can also update TAR.LZMA and TLZ files. It validates the compressed stream and
the decompressed TAR before an update. The Writer supports compression levels
1 through 9.

BZIP2 creation writes one compressed stream per selected input file. The output
uses concatenated BZIP2 streams and the selected block level from 1 to 9.

Zstandard creation writes one standard frame per selected input file. The output
uses concatenated frames and supports compression levels -5 through 22.

XZ creation writes one XZ stream per selected input file. The output uses
concatenated streams and supports no check, CRC32, CRC64, or SHA-256 checks.

LZMA-alone creation writes one stream for one selected input file and supports
compression levels 1 through 9.

Unix compress creation writes one `.Z` stream for one input file. It supports
block and non-block modes with code widths from 9 through 16 bits.

The ar Writer creates GNU long-name tables or BSD extended-name records. Ar
members are flat files, so the Writer does not create directory entries. Symbol
index records are not exposed as members; a rewritten archive omits them. Run
the platform `ranlib` tool when the archive must be used as a linkable library.

`peare_writer_add_files_to_ex()` uses one `peare_writer_create_options` value
for a new output. Set its format, variant, and filesystem IDs. Set the options
pointer to null when you update an existing target. The filesystem query returns
choices for disk images. The older `peare_writer_add_files_to()` entry point
remains available for clients that do not need a filesystem ID. Use the batch
API to create floppy images, raw disks, or new virtual disks.

Each input is a pair of UTF-8 source and entry paths:

```c
typedef struct peare_writer_input_file {
    const char *source_path_utf8;
    const char *entry_name_utf8;
} peare_writer_input_file;

typedef struct peare_writer_create_options {
    const char *format_id_utf8;
    const char *variant_id_utf8;
    const char *filesystem_id_utf8;
} peare_writer_create_options;
```

For a new archive or FAT image, pass its output path, format and variant IDs,
target index zero, and the complete input-file array. For a new virtual disk,
also pass a filesystem ID. For an existing container, set create options to
null and pass the selected target index. Each input gives a UTF-8 source path
and destination entry name. The call creates or updates the target and commits
it before it returns success.

## Generic update operations

The update ABI exposes create, update, rename, and delete operations. Reading
content remains the Opener's responsibility. Existing `add_*` functions remain
available for ABI compatibility. New clients should use the generic capability,
target, and update functions below.

```c
int peare_writer_can_add_files(const char *container_path_utf8);
size_t peare_writer_add_target_count(const char *container_path_utf8);
peare_writer_status peare_writer_get_add_target_name(
    const char *container_path_utf8, size_t target_index,
    char *name_buffer, size_t name_buffer_size);
peare_writer_status peare_writer_get_update_capabilities(
    const char *container_path_utf8, size_t target_index,
    peare_writer_update_capabilities *out_capabilities);
size_t peare_writer_update_target_count(const char *container_path_utf8);
peare_writer_status peare_writer_get_update_target_name(
    const char *container_path_utf8, size_t target_index,
    char *name_buffer, size_t name_buffer_size);
peare_writer_status peare_writer_update_begin(
    peare_writer_handle writer, const char *container_path_utf8);
peare_writer_status peare_writer_update_begin_target(
    peare_writer_handle writer, const char *container_path_utf8,
    size_t target_index);
peare_writer_status peare_writer_create_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
peare_writer_status peare_writer_create_empty_file(
    peare_writer_handle writer, const char *entry_name_utf8);
peare_writer_status peare_writer_create_directory(
    peare_writer_handle writer, const char *entry_name_utf8);
peare_writer_status peare_writer_replace_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
peare_writer_status peare_writer_replace_entry_by_id(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8, int64_t entry_id);
peare_writer_status peare_writer_rename_entry(
    peare_writer_handle writer, const char *entry_name_utf8,
    const char *new_entry_name_utf8);
peare_writer_status peare_writer_rename_entry_by_id(
    peare_writer_handle writer, const char *entry_name_utf8,
    const char *new_entry_name_utf8, int64_t entry_id);
peare_writer_status peare_writer_delete_entry(
    peare_writer_handle writer, const char *entry_name_utf8, int recursive);
peare_writer_status peare_writer_delete_entry_by_id(
    peare_writer_handle writer, const char *entry_name_utf8,
    int recursive, int64_t entry_id);
peare_writer_status peare_writer_add_files_begin(
    peare_writer_handle writer, const char *container_path_utf8);
peare_writer_status peare_writer_add_files_begin_target(
    peare_writer_handle writer, const char *container_path_utf8,
    size_t target_index);
peare_writer_status peare_writer_add_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
peare_writer_status peare_writer_update_finish(peare_writer_handle writer);
```

On Windows, target discovery checks for a sibling `.peare-journal`. If it
contains a valid interrupted transaction, Peare restores the original image
before it scans for targets. A malformed or mismatched journal produces no
writable targets and remains in place. Discovery does not write to the image
when no journal exists. New journals store a SHA-256 checksum for each backup
range. Recovery also accepts legacy `PEAREJ01` journals, which have no range
checksums.

`peare_writer_get_update_capabilities()` returns a bit mask of operations
supported for the selected target. The current bits are `CREATE_FILE`,
`CREATE_EMPTY_FILE`, `CREATE_DIRECTORY`, `UPDATE_FILE`, `RENAME_ENTRY`, and
`DELETE_ENTRY`. Clients must call only operations reported by this function.
An unsupported operation returns `PEARE_WRITER_UNSUPPORTED_OPERATION` and does
not abort the active update. `recursive` must be zero or one; it controls whether
deleting a directory may also delete its contents.

For a container with no selectable volume, the update target count is one and
its name is `Archive`. Disk-image targets use the discovered volume names. The
legacy add-target functions remain available for existing clients.

ZIP update targets report all six CUD capabilities. ZIP replacement writes a
new member and removes the previous member from the central directory. Rename
preserves the compressed member data and updates its local and central records.
Deleting a directory requires `recursive` when it contains entries. ZIP archive
creation accepts generic file, empty-file, and directory creation.

TAR targets report all six CUD capabilities. TAR updates can create regular
files and directories, replace files, rename files and directory subtrees, and
delete files or directories. A non-empty directory requires recursive delete.
All supported CPIO variants report all six CUD capabilities. They support regular
files, empty files, directories, file replacement, entry rename, and file or
recursive directory deletion. Updates retain the source variant and preserve existing
special entries and opaque trailing data.
IWAD and PWAD targets report create-file, create-empty-file, update-file, rename,
and delete capabilities. Doom WAD has no directory entries. Lump names are limited
to 1-8 printable ASCII characters. The `*_entry_by_id()` operations target one
directory record, so repeated lump names remain individually editable. Name-only
operations reject ambiguous lump names.
Supported FAT image targets report all six CUD capabilities. They support
regular files and directories, file replacement, entry rename, and file or
recursive directory deletion. The legacy `peare_writer_can_add_files()` is
equivalent to checking whether a target reports `CREATE_FILE`.

Supported exFAT targets report all six CUD capabilities. They support regular
files and directories, file replacement, entry rename, and file or recursive
directory deletion. Tests cover raw images and VHD, VHDX, and VMDK wrappers.

Supported NTFS targets report all six CUD capabilities. They support regular
files and directories, file replacement, entry rename, and file or recursive
directory deletion. The backend uses the bundled NTFS-3G library. Current real
sample coverage uses a Lumia NTFS volume in a VHDX copy and an InTouch NTFS
volume in a VHD copy. The tests check each final Opener root snapshot and source
image hash. They do not cover all NTFS versions, compressed or encrypted data,
reparse points, hard links, or alternate data streams. This coverage does not
establish production readiness.

Updates are transactional: the original path is replaced only when
`peare_writer_update_finish()` succeeds. The generic `peare_writer_finish()`
also finishes create operations.

The ZIP backend reads and updates ZIP64 end records and per-entry ZIP64 extra
fields. It stages updates separately, then commits the completed archive through
an atomic replacement. It preserves untouched members and central records.
Rename copies compressed member bytes without recompression and updates local and
central records. Replace appends a new member and removes the old record. Delete
removes entries from the central directory; recursive directory deletion also
removes all members below that path. Existing archive comments remain. Updates
preserve the compressed data and method of untouched members. New archives use
the selected Store, Deflate, BZip2, LZMA, XZ, Zstandard, or PPMd method. New members in existing ZIP
archives use Deflate. New ZIP64 records are written when required. Multi-disk
archives and unsafe new entry names are rejected.

The TAR backend writes USTAR regular-file and directory entries. It creates a
new archive or updates an existing archive. Updates preserve untouched payload
bytes, rewrite USTAR headers for renamed entries, and publish the staged archive
only after all selected changes succeed. Multi-disk archives, GNU base-256
numeric fields, and names that do not fit USTAR are not supported for new or
renamed entries.

Non-empty TAR updates require valid USTAR or V7 headers and two zero end
blocks. An empty TAR has no signature. The API accepts one only when its file
name ends in `.tar` and its size is 10 KiB or less.

The FAT backend creates and updates regular files and directories in FAT12,
FAT16, and FAT32 volumes. It supports file replacement, file and directory
rename, and file or recursive directory deletion.
The NTFS backend provides the same CUD operations for supported regular files
and directories. The backend stages all changes before it commits the disk
image. Real-sample tests cover Lumia VHDX and InTouch VHD copies.
The create API can make fixed or dynamic VHD and VHDX disks. It can also make
monolithic sparse and split sparse VMDK disks. A new split sparse VMDK uses one
external extent, limited to 2 GiB. It formats each new disk with FAT12, FAT16,
FAT32, exFAT, or NTFS and adds the selected files before it publishes the descriptor.
It supports standalone images, fixed VHD, and standalone dynamic Type 3 VHD.
It also supports fixed and dynamic VHDX images.
Supported VMDK layouts include standalone monolithicSparse and
`twoGbMaxExtentSparse` extents with versions 1 and 2.
Standalone version 3 `streamOptimized` VMDKs are also supported.
The Writer converts an updated streamOptimized disk to version 1
`monolithicSparse` with redundant grain directories before publication.
Version 2 zeroed-grain entries read as zero and can receive allocated data.

Dynamic VHD, VHDX, and VMDK images can contain FAT, exFAT, or NTFS partitions in MBR or GPT
layouts. GPT headers and partition tables must pass checksum, geometry, and
copy-consistency checks. The Writer rejects malformed or overlapping MBR
entries and extended partitions.

Raw images and fixed VHD images can contain standalone FAT, exFAT, or NTFS
volumes, or validated MBR and GPT disks. The caller must select a target when
the disk contains more than one supported volume. The GUI lists the discovered
targets.

FAT32 volumes with mirroring disabled use the FAT selected by `BPB_ExtFlags`.
The Writer rejects invalid active-FAT indices and reserved flag bits.
Before exposing a target and again before staging, it compares mirrored FAT
copies in bounded chunks. It does not expose targets with different copies and
rejects changes detected before staging. Target discovery does not scan FAT
payloads.

The 1.84 MiB OS/2 XDF floppy is a validated exception. XDF stores an 8-sector
compatibility image in the area that a standard FAT12 layout treats as the
second FAT. The Writer updates only the primary FAT and checks that the full
compatibility area is unchanged before it publishes the staged image. Other
XDF geometries are not supported.

VMDK updates validate embedded or external descriptors, both grain directories,
and separate redundant grain-table copies. The directories cannot point to the
same grain table. The Writer rejects images without redundant directories or
with missing grain-table pointers.

The Writer changes existing grains only in the staged copy. It appends new
grains and updates both grain-table copies. It treats zero GTEs in present
tables as unallocated. It rejects parent disks, `VMFS` extents, and unsupported
compressed VMDK variants. It also rejects split sparse extents without valid
redundant grain metadata.

File-backed `monolithicFlat` and `twoGbMaxExtentFlat` descriptors with `RW FLAT`
extents are supported. Nonzero flat-file sector offsets are supported too.
The Writer rejects device-backed extents, mixed flat and sparse layouts, and
read-only or inaccessible extents.

Flat and sparse split updates stage copies of changed extents. The Writer
switches the descriptor only after validation. It retains the original extents.
For flat updates, it copies each complete backing file before it changes the
descriptor reference.

The Writer flushes changed extent data and the staged file-system image before
publication. It flushes VMDK descriptors after their referenced extents.
POSIX builds also sync the containing directory before they switch the
descriptor. Windows does not request write-through for the atomic rename.
Power loss can therefore lose the namespace update on Windows.

Peare serializes FAT updates with an interprocess `.peare-fat.lock`.
Split VMDK updates also use a transaction lock. They refuse to commit if a
VMware `.lck` file appears during the update.

FAT updates hold a write guard on the source while staging and applying changes.
They check the source digest while the guard is active. They check it again
before atomic replacement.
On Windows, the guard blocks write handles but permits readers.
The Writer releases it just before replacement because Windows cannot replace
an open source file. On POSIX, the guard coordinates cooperating writers only.
External programs must not change the image during the final commit window.

Peare does not remove stale `.peare.lock` or `.peare-fat.lock` files.
Inspect a stale lock before you remove it.

Dynamic VHD updates append allocated blocks and refresh the BAT and footer in
the staged copy. Block sizes must be powers of two from 64 KiB to 64 MiB.
The BAT cannot have more than 1,000,000 entries. The Writer rejects
differencing Type 4 VHDs.

VHDX region-table copies must match and pass checksum and structure checks.
The Writer checks GPT copies independently.
Partition identity, geometry, and names must match. Attributes can differ.
VHDX logical sectors can be 512 or 4096 bytes.
The Writer supports MBR and GPT partition addressing.
It rejects images that need log replay, use differencing, or have other sector
geometry.

The Writer creates missing parent directories and supports UTF-8 long names.
It validates fixed VHD footer metadata and preserves it byte-for-byte.
It formats new disks with FAT12, FAT16, FAT32, exFAT 1.0, or NTFS.
It does not write ext, JFS, XFS, Btrfs, VDI, or QCOW images.

FAT updates are staged as a complete temporary copy. Peare validates and edits
that copy, checks that the source image has not changed, then commits the new
image through an atomic replacement. An aborted or failed operation leaves the
original image unchanged. This staging requires temporary storage at least as
large as the image, in addition to space for the replacement during commit.
FatFs R0.16 is included under its accompanying permissive license.

NTFS updates write a rollback journal before they change the source image.
Peare flushes committed image ranges before it marks the journal complete.
On the next open, Peare restores an incomplete update before it exposes a target.

## ZIP

The ZIP-specific API can create a new archive transactionally. The destination
is replaced only after its finish call succeeds. An abandoned active writer is
cancelled by `peare_writer_destroy()` or `peare_writer_abort()`.

New entries use UTF-8 names and the selected ZIP Store, Deflate, BZip2, LZMA, XZ, Zstandard, or PPMd method. File
input is streamed. The Writer does not need to hold an entire file in memory.
New entries use ZIP32 size fields when possible.
ZIP64 fields store offsets and archive records when required.

Entry names use `/` separators. Absolute paths, drive-prefixed paths, `.` and
`..` path segments, empty names, and duplicate normalized names are rejected.
Directory names receive a trailing `/` automatically.

## ZIP-specific exports

```c
peare_writer_status peare_writer_create(peare_writer_handle *out_writer);
void peare_writer_destroy(peare_writer_handle writer);

peare_writer_status peare_writer_zip_begin(
    peare_writer_handle writer, const char *output_path_utf8);
peare_writer_status peare_writer_zip_add_directory(
    peare_writer_handle writer, const char *entry_name_utf8);
peare_writer_status peare_writer_zip_add_buffer(
    peare_writer_handle writer, const char *entry_name_utf8,
    const uint8_t *bytes, size_t length);
peare_writer_status peare_writer_zip_add_file(
    peare_writer_handle writer, const char *source_path_utf8,
    const char *entry_name_utf8);
peare_writer_status peare_writer_zip_finish(peare_writer_handle writer);
void peare_writer_abort(peare_writer_handle writer);
const char *peare_writer_last_error(peare_writer_handle writer);
const char *peare_writer_status_message(peare_writer_status status);
```

`peare_writer_last_error()` returns a handle-owned UTF-8 message, valid until the
next Writer API call on that handle. Status messages are static strings.

## Lifetime

```c
peare_writer_handle writer = NULL;
peare_writer_status status = peare_writer_create(&writer);
if (status == PEARE_WRITER_OK)
    status = peare_writer_zip_begin(writer, "output.zip");
if (status == PEARE_WRITER_OK)
    status = peare_writer_zip_add_file(writer, "input.txt", "folder/input.txt");
if (status == PEARE_WRITER_OK)
    status = peare_writer_zip_finish(writer);
if (status != PEARE_WRITER_OK)
    peare_writer_abort(writer);
peare_writer_destroy(writer);
```
