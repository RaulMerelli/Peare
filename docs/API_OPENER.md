# PeareOpener C API

Header:

```c
#include <peare/peare_opener.h>
```

The DLL is stateful: an Opener handle maintains the session for the open file.

## Exports

```c
peare_status peare_opener_create(peare_opener_handle *out_opener);
void peare_opener_destroy(peare_opener_handle opener);

peare_status peare_opener_open_file(
    peare_opener_handle opener,
    const char *path_utf8);

peare_status peare_opener_open_buffer(
    peare_opener_handle opener,
    const uint8_t *bytes,
    size_t length,
    const char *source_name_utf8);

peare_status peare_opener_close(peare_opener_handle opener);

peare_status peare_opener_get_folder_count(
    peare_opener_handle opener,
    size_t *out_count);

peare_status peare_opener_get_folder_type(
    peare_opener_handle opener,
    size_t folder_index,
    peare_blob *out_type_utf8);

peare_status peare_opener_get_resource_count(
    peare_opener_handle opener,
    size_t folder_index,
    size_t *out_count);

peare_status peare_opener_open_resource_at(
    peare_opener_handle opener,
    size_t folder_index,
    size_t resource_index,
    peare_resource_handle *out_resource);

peare_status peare_opener_open_resource(
    peare_opener_handle opener,
    size_t folder_index,
    const char *identifier_utf8,
    const char *preferred_language_utf8,
    peare_resource_handle *out_resource);

peare_status peare_opener_find_resource(
    peare_opener_handle opener,
    const char *type_utf8,
    const char *identifier_utf8,
    const char *preferred_language_utf8,
    peare_resource_handle *out_resource);

void peare_resource_destroy(peare_resource_handle resource);

peare_status peare_resource_get_converted_extensions(
    peare_resource_handle resource,
    peare_blob_array *out_extensions_utf8);

peare_status peare_resource_convert(
    peare_resource_handle resource,
    const char *extension_utf8,
    peare_blob_array *out_files);

void peare_resource_conversion_array_free(peare_blob_array *array);

peare_status peare_resource_get_payload(
    peare_resource_handle resource,
    peare_blob *out_payload);

peare_status peare_resource_get_context(
    peare_resource_handle resource,
    peare_resource_context *out_context);

void peare_blob_free(peare_blob *blob);
void peare_resource_context_free(peare_resource_context *context);
const char *peare_status_message(peare_status status);
```

## Supported container formats

The Opener recognizes PE, NE, LE, LX, XEX, XBE, XUIZ, LIVE/PIRS, CON, IBM/Microsoft OS/2 PACK/PACK2, Microsoft Compress SZDD, Microsoft Cabinet CAB, ZIP, TAR, SDI and XVA deployment images, Siemens ProSave IMG firmware containers, Siemens FWF OMS firmware archives, Linux swap, Linux LVM2, Linux MD RAID1, Windows Dynamic Disk/LDM, Windows Registry hive and BCD BootConfig files, filesystem images (including FAT/exFAT/NTFS/ext/XFS/Btrfs/SquashFS/HFS+/UDF), raw Mode2 optical BIN images, raw partitioned disk images, and DMG/VMDK/VHD/VDI/VHDX virtual disks. Every eligible contained payload is passed back through the same `ModuleFactory` dispatch by `OpenerSession`, so any supported format can be opened at any nesting level without container-specific opener calls. Registry hives expose `REG_KEY` container entries and `REG_VALUE` value payload resources; BCD stores expose `BCD_OBJECT` containers and `BCD_ELEMENT` semantic payloads. OS/2 PACK members are exposed as `OS2_PACK_FILE` resources containing the byte-perfect decompressed payload. SZDD archives expose one `SZDD_FILE` resource containing the exact decompressed payload; CAB archives expose `CAB_FILE` resources for uncompressed, MSZIP and LZX folders. ZIP archives expose `ZIP_FILE` resources for stored and deflate entries; ZIP64 and encrypted entries are not yet supported. TAR archives expose `TAR_FILE` resources for regular files and directories, with hard links resolved when the target appears first and symlinks skipped. Mode2 optical BIN images are exposed through the same ISO/UDF resources after stripping each 2352-byte raw sector to its 2048-byte payload. Raw partitioned disk images expose MBR/GPT/APM partitions as nested `DISK_PARTITION` payloads. SDI images expose their section blobs (`PART`, `WIM`, etc.) as nested payloads, XVA appliances reconstruct chunked VDI disks and expose their partitions, LVM2 physical volumes expose readable logical volumes as nested payloads, Linux MD RAID1 members expose `LINUX_RAID_VOLUME` nested payloads from v0.90 or v1.x superblocks, and Windows LDM dynamic disks expose concatenated and striped volumes as `LDM_VOLUME` nested payloads when their extents are present in the opened disk image. The OS/2 PACK implementation supports the known `A5 96` PACK header variants, chained members, compact DOS 8.3 names, extended headers, IBM's fixed-width 12-bit LZW stream with least-recently-used leaf replacement, and PACK2 FTCOMP `fT19` entropy/LZ streams (`A5 96 FD FF`).

## Minimal lifetime

```c
peare_opener_handle opener = NULL;
peare_status st = peare_opener_create(&opener);
if (st != PEARE_STATUS_OK) return 1;

st = peare_opener_open_file(opener, "APP.EXE");
if (st != PEARE_STATUS_OK) {
    peare_opener_destroy(opener);
    return 1;
}

/* navigation */

peare_opener_destroy(opener);
```

`peare_opener_destroy()` closes and releases the session. `peare_opener_close()` closes the container but preserves the handle for a later open operation.

## Navigation

1. `get_folder_count()`;
2. for each folder, `get_folder_type()`;
3. `get_resource_count()`;
4. `open_resource_at()`.

## Opening by identifier

Numeric identifiers are normally represented as strings prefixed with `#`, for example `#101`.

`preferred_language_utf8` may be `NULL`. When provided, the Opener prefers the matching language variant while preserving the original order as fallback.

## Payload byte-perfect

`peare_resource_get_payload()` returns a Peare-allocated copy of the reconstructed original payload. The content is not modified by the Decoder or the GUI.

## Context

`peare_resource_get_context()` returns the container format, platform, type, identifier, language, codepage, and origin data. Always release it with `peare_resource_context_free()`.

## Example: extraction and decoding

```c
peare_resource_handle resource = NULL;
peare_blob payload = {0};
peare_resource_context context = {0};

st = peare_opener_open_resource_at(opener, 0, 0, &resource);
if (st == PEARE_STATUS_OK)
    st = peare_resource_get_payload(resource, &payload);
if (st == PEARE_STATUS_OK)
    st = peare_resource_get_context(resource, &context);

/* pass the payload and textual context to the Decoder */

peare_resource_context_free(&context);
peare_blob_free(&payload);
peare_resource_destroy(resource);
```

## Module conversion

For synthetic resources representing an entire module, the Opener may expose container-level conversions. For `PE_MODULE`, `peare_resource_convert()` reconstructs a file-layout PE from the original loaded image. This operation belongs to the Opener rather than the Decoder because it uses module headers, sections, RVAs, and alignments.


## Siemens firmware containers

`SIEMENS_IMG` validates the ProSave footer, decodes section/chunk streams, supports zero-section common-file images, and exposes reconstructed `NK.bin` plus diagnostic section areas. `SIEMENS_FWF` walks the OMS token stream and exposes reconstructed `NK.bin`, raw flash images, FSF volumes, image-part payloads, and nested OMS streams. Modules parse only their own container; nested opening remains centralized in `OpenerSession`.
