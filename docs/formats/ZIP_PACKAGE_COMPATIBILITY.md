# ZIP Package Compatibility

The Reader and Writer detect standard ZIP records. They do not select a ZIP
backend from the file extension. The same archive operations therefore apply to
ZIP-based files such as JAR, APK, EPUB, DOCX, and XPI.

The extensions listed in Supported Formats are common examples, not an
allowlist. Any standard ZIP archive can use the same container operations,
regardless of its file extension.

The Writer can create Store, Deflate, BZip2, LZMA, XZ, Zstandard, PPMd, and ZIPX JPEG entries. It preserves
the original compressed data for untouched entries. New or replaced entries
use the selected method. ZIP methods not listed here are not available for new
entries yet. The method-by-method status is in [ZIP Writer Methods](../testing/ZIP_WRITER_METHODS.md).

The Writer changes the ZIP container only. It does not update package manifests,
relationships, signatures, or catalogs. An edited package can fail signature
checks or package validation. Applications can also ignore added files that are
not listed in package metadata.

This applies to signed JAR, APK, AAB, IPA, APPX, MSIX, APPXBUNDLE, and XAP
files. Peare does not re-sign these packages.
