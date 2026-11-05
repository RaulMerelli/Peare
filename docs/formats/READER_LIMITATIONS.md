# Archive Reader Limits

This document describes the archive features that Peare does not support. It
covers 7-Zip and RAR. The format matrix lists the other supported formats.

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

## RAR

Peare supports RAR4 only. It returns data for stored members and LZ methods m1
through m5. It also decodes solid LZ members.

Peare can list members that use PPMd or filter methods. It cannot return their
decoded data. Peare does not join split members or decrypt encrypted headers or
data. A supplied password does not change these limits.

RAR4 can store a Unicode name as a token stream. Peare uses the legacy filename
when it cannot decode that stream. Peare does not support RAR5.

## UUencode

Peare reads one classic `begin` block with one file. The header must start at
the first line. Peare does not read `begin-base64` blocks or text before the
header.
