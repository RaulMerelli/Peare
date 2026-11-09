# ZIP Writer Methods

This checklist tracks write methods that match the ZIP Reader.
`SUPPORTED_FORMATS.md` must list only verified support.

| Method ID | Name | Scope | State |
|---:|---|---|---|
| 0 | Store | ZIP | Complete. ABI and GUI CUD tests pass. |
| 1 | Shrink | ZIP | Pending. |
| 2-5 | Reduce 1-4 | Reader claim needs investigation. The current ZIP module rejects these IDs. | Pending |
| 6 | Implode | ZIP | Pending. |
| 8 | Deflate | ZIP | Complete. ABI and GUI CUD tests pass. |
| 9 | Deflate64 | ZIP | Pending. |
| 10 | DCL Implode | ZIP | Pending. |
| 12 | BZip2 | ZIP | Complete. ABI, external-tool, and GUI CUD tests pass. |
| 14 | LZMA | ZIP | ABI, independent 7-Zip/WinRAR, and GUI Tree/Details checks pass. |
| 93 | Zstandard | ZIPX | ABI, Peare Reader, WinRAR test/extraction, and GUI Tree/Details CUD pass. 7-Zip 19 does not support this method. |
| 94 | MP3 | ZIPX | Pending. |
| 95 | XZ | ZIPX | ABI, Peare Reader, independent 7-Zip/WinRAR, and GUI Tree/Details checks pass. |
| 96 | JPEG | ZIPX | Complete. ABI round-trip through Peare Reader passes with an external JPEG sample. The Reader also opens an authentic WinRAR-created method-96 sample. |
| 97 | WavPack | ZIPX | Pending. |
| 98 | PPMd | ZIPX | ABI file/buffer creation, Peare Reader, WinRAR test/extraction, and GUI Tree/Details CUD pass. |

Each encoder must write valid local and central records, checksums, size fields,
and method-specific metadata. Tests must compare payload bytes through the public
Reader ABI and validate output with an independent decoder when one supports the
method. Media methods must reject incompatible input types.
