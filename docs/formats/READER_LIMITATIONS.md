# Archive Reader Limits

This document describes archive features that Peare does not support. The
format matrix lists the supported formats.

## Nintendo Switch NUP

Peare reads unencrypted NUP version 1 package trees with raw and Zstandard file
payloads. It skips unknown metadata TLVs. It does not decrypt game content.
The public draft and its reference packer use different IDs for text metadata
TLVs. Peare reads both numberings.

## XNA / MonoGame XNB

Peare reads XNB versions 4 and 5 with uncompressed, LZX, and raw LZ4 content.
It reports the content-reader table and preserves the serialized root asset.
It previews Color Texture2D assets. It does not deserialize other root asset
types into their game-level structures.

## DirectX Shader Container

Peare validates the DXBC header, chunk offsets, sizes, and non-overlap. It
exposes raw chunks and does not validate the container checksum or decode shader
instructions.

## Quake WAD2

The reader lists lumps and returns their stored bytes. It does not decompress
lumps marked with the WAD2 LZSS flag. The distributed Quake 1.06 sample uses
uncompressed lumps.

## Half-Life WAD3

The reader lists lumps and returns their stored bytes. It does not decompress
lumps marked with the WAD3 LZSS flag. The tested `decals.wad` sample uses
uncompressed lumps.

## Xbox 360 XDBF/GPD

Peare reads XDBF version 1.0 record tables, shows the GPD title string, and
previews image records. Other record payloads remain raw. Peare does not decode
achievement, setting, or synchronization record structures.

## Microsoft WIM

Peare reads split WIM sets when all parts are in one directory. The files must
use the standard names `base.swm`, `base2.swm`, `base3.swm`, and so on. Open any
part in the set. Peare checks the part numbers, total count, and shared GUID.
Tests cover a two-part set made by wimlib 1.14.5.

## Compiled HTML Help

Peare reads ITSF version 2 and 3 CHM files that use the Microsoft LZX transform.
Help 2 containers and other compression transforms are not supported. An IE4
Platform Preview 2 sample verifies the ITSF v2 reader path.

## NSIS

Peare reads the standard NSIS 1.x, 2.x, and 3.x installer layouts. Tests cover
NSIS 1.98 with zlib and bzip2, NSIS 2.46 and 2.51, and NSIS 3.13 with zlib,
bzip2, and LZMA in solid and non-solid modes. Custom forks and modified NSIS
builds are not validated. Peare limits each decoded data stream to 1 GiB.

## 7-Zip

Peare reads 7z archives that use the codecs and filters listed below.

| Type | Supported methods |
| --- | --- |
| Codec | Copy, Deflate, Deflate64, BZip2, LZMA, LZMA2, PPMd |
| Filter | Delta, BCJ x86, BCJ2, PPC, IA-64, ARM, ARM-Thumb, SPARC, ARM64, RISC-V, Swap2, Swap4 |
| Encryption | 7z AES-256 for file data and headers |

Peare also reads solid folders, compressed headers, and numbered split volumes.
The first split volume must use the `.001` suffix. Later volumes must use the
same base name and the next numbers.

Peare does not decode a codec or filter that is not in the table. The 7z reader
accepts up to 16 coders in one folder and up to 1,000,000 entries in one archive.

See the [7-Zip method list](https://github.com/ip7z/7zip/blob/main/DOC/Methods.txt)
for method names and IDs.

## WinImage IMZ

Peare reads an unencrypted ZIP-compatible IMZ that contains one standard-capacity
raw floppy image named with the `.ima` or `.img` suffix. Encrypted images and
archives with multiple floppy images are not supported.

## Amiga ADF

The Reader supports 880 KiB OFS and FFS floppy images, including images with
international and directory-cache flags. It reads directory hash chains and
does not use directory-cache blocks. Amiga file and directory links are not
listed as resources.

ADZ support is limited to one GZIP member that contains a standard 880 KiB ADF
with a valid Amiga DOS file system. Extended ADF images and multi-member ADZ
streams are not supported.

## Android APK signing metadata

Peare reports APK Signature Scheme v2, v3, and v3.1 signer counts,
leaf-certificate SHA-256 fingerprints, common names, and v3 key lineage. It does
not verify APK signatures. APKS and AAB files use generic ZIP browsing.

## RAR

Peare reads RAR4 stored members, LZ methods m1 through m5, PPMd data, and
filter records. It also reads solid LZ members.

Peare joins split members in both `.partN.rar` and `.rar`/`.rNN` volume sets.
It decrypts encrypted headers and member data when the caller supplies the
correct password.

Peare reads RAR5 algorithm version 0 with methods 0 to 5. It reads solid
streams, encrypted headers, encrypted file data, and split members in `.partN.rar`
volume sets. Open the first volume to read the set. Peare reads the Delta, E8,
E8E9, and ARM filters defined by RAR5. ABI tests compare all four filter paths
with 7-Zip output, including a Libarchive ARM-filter sample.

Peare reads RAR 7 algorithm version 1, including extended distance codes and
fractional dictionary sizes. An ABI test reads a real sample with a 6 GiB
dictionary. It checks 4,563,402,752 output bytes and CRC32 `8E9B8BF4`. Peare
keeps the full decoded member in memory. Large members can need several GiB of
RAM.

## XAR

Peare reads stored, zlib, and bzip2 file data. It does not read LZMA-encoded
members. It does not expose symbolic links, special files, or extended attributes.
The compressed or expanded table of contents cannot exceed 256 MiB. Peare verifies
SHA-1, MD5, SHA-256, and SHA-512 checksums.

## Microsoft XBCOMPRESS

Peare reads LZMS and MSZIP streams. It decodes each stream into memory. Large
files can need several GiB of RAM.

## XZ

Peare reads XZ streams that use one LZMA2 filter per block. It does not read
XZ blocks that use Delta or BCJ filters.

## LZMA-alone

Peare reads `.lzma` streams with a declared unpacked size and streams that use
the unknown-size marker and an end marker. The LZMA-alone format does not
provide a checksum for the decoded data.

## Zstandard

Peare reads standard Zstandard frames that do not require an external
dictionary. The Opener ABI does not accept dictionary data. Frames that require
a history window larger than 2 GiB on 64-bit builds are not supported.

## RPM

Peare reads RPM packages with CPIO payloads and the supported compressors listed
in the format matrix. It does not verify RPM package signatures. Payloads that
use another compressor or the large-file `07070X` CPIO extension are not read.

The Writer creates unsigned RPM v3 packages. Its MD5 digest detects data changes,
but it does not authenticate the package source.
