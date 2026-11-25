# ZIP Package Compatibility

The Reader and Writer detect standard ZIP records. They do not select a ZIP
backend from the file extension. The same archive operations therefore apply to
ZIP-based files such as JAR, APK, EPUB, DOCX, and XPI.

The extensions listed in Supported Formats are common examples, not an
allowlist. Any standard ZIP archive can use the same container operations,
regardless of its file extension.

The Writer can create entries with the methods in this table. It preserves the
original compressed data for untouched entries. New and replaced entries use
the selected method.

| Method ID | Name | Scope | State |
|---:|---|---|---|
| 0 | Store | ZIP | Complete. ABI and GUI CUD tests pass. |
| 1 | Shrink | ZIP | Complete. ABI CUD passes on local ArcaOS samples; GUI Tree/Details CUD passes. 7-Zip validates updated archives. |
| 2-5 | Reduce 1-4 | ZIP | Complete. Create/read round-trips pass for all four factors; independent HWZIP 2.4 extraction passes. GUI Tree/Details CUD passes for all four methods. |
| 6 | Implode | ZIP | Complete. ABI create/read round-trip and GUI Tree/Details CUD pass. |
| 8 | Deflate | ZIP | Complete. ABI and GUI CUD tests pass. |
| 9 | Deflate64 | ZIP | Complete. Create/read round-trip and GUI Tree/Details CUD pass. |
| 10 | DCL Implode | ZIP | Complete. ABI create/read round-trip and GUI Tree/Details CUD pass. |
| 12 | BZip2 | ZIP | Complete. ABI, external-tool, and GUI CUD tests pass. |
| 14 | LZMA | ZIP | Complete. ABI create/read, independent 7-Zip/WinRAR, and GUI Tree/Details CUD checks pass. |
| 93 | Zstandard | ZIPX | Complete. ABI create/read, Peare Reader, WinRAR test/extraction, and GUI Tree/Details CUD pass. 7-Zip 19 does not support this method. |
| 94 | MP3 | ZIPX | Complete. Real WinZip sample payload is preserved by the ABI Writer; GUI Tree/Details CUD passes. |
| 95 | XZ | ZIPX | Complete. ABI create/read, Peare Reader, independent 7-Zip/WinRAR, and GUI Tree/Details CUD checks pass. |
| 96 | JPEG | ZIPX | Complete. ABI create/read and full CUD pass; GUI Tree/Details CUD preserves the JPEG payload. The Reader also opens an authentic WinZip-created method-96 sample. |
| 97 | WavPack | ZIPX | Complete. ABI create/read round-trip and GUI Tree/Details CUD pass using a real WavPack sample. |
| 98 | PPMd | ZIPX | Complete. ABI file/buffer creation, Peare Reader, WinRAR test/extraction, and GUI Tree/Details CUD pass. |

Each encoder must write valid local and central records, checksums, size fields,
and method-specific metadata. Tests must compare payload bytes through the public
Reader ABI and validate output with an independent decoder when one supports the
method. Media methods must reject incompatible input types.

The Writer changes the ZIP container only. It does not update package manifests,
relationships, signatures, or catalogs. An edited package can fail signature
checks or package validation. Applications can also ignore added files that are
not listed in package metadata.

This applies to signed JAR, APK, AAB, IPA, APPX, MSIX, APPXBUNDLE, and XAP
files. Peare does not re-sign these packages.
