# Third-party notices

Peare is distributed under the GNU Affero General Public License, version 3.
See [LICENSE](LICENSE) for its terms.

## Third-party dependencies

| Dependency | Use in Peare | Notice or license |
|---|---|---|
| Qt | GUI toolkit and platform services | The license depends on each Qt module and build. Review the notices supplied with the Qt distribution and the [official licensing terms](https://www.qt.io/development/qt-licensing). |

## Original third-party code

These entries identify upstream code that Peare includes or uses without a
language port. See each notice for the exact license and source revision.

| Project or source | Use in Peare | Notice or license |
|---|---|---|
| FatFs R0.16 | FAT file-system writer | [`src/writer/third_party/fatfs/LICENSE.txt`](src/writer/third_party/fatfs/LICENSE.txt) |
| lwext4 | Ext2/Ext3 formatter and file-system writer; only BSD-3-Clause files are included | [`src/writer/third_party/lwext4/LICENSE-BSD-3-Clause.txt`](src/writer/third_party/lwext4/LICENSE-BSD-3-Clause.txt); source revision `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5` |
| Google WOFF2 decoder | WOFF2 font decompression | [`src/decoder/font/third_party/woff2/LICENSE`](src/decoder/font/third_party/woff2/LICENSE); revision `7b67e12355fe7da148db5c19d4261f0a498a9537` |
| Google Brotli decoder | WOFF2 Brotli decompression | [`src/decoder/font/third_party/brotli/LICENSE`](src/decoder/font/third_party/brotli/LICENSE); revision `d89c4b1c52e25ddafe047bc78bbee5c1e24b1809` |
| libeot 0.01 | EOT MicroType Express decompression and CTF reconstruction | [`src/decoder/font/third_party/libeot/LICENSE`](src/decoder/font/third_party/libeot/LICENSE), [`PATENTS`](src/decoder/font/third_party/libeot/PATENTS); revision `0407abddc581d32e9871ee41535183ee1d924d85` |
| bzip2 1.0.8 | ZIP method 12 compression in `PeareWriter`; legacy randomization table for pre-1.0 BZIP2 blocks in `PeareOpener` | [`src/writer/third_party/bzip2/LICENSE`](src/writer/third_party/bzip2/LICENSE); [official source archive](https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz) |
| LZMA SDK 26.05 | ZIP methods 14 (LZMA) and 95 (XZ) encoders; LZMA, LZMA2, PPMd7, PPMd8, Delta, and BCJ archive decoders | Public domain. Encoder and decoder source is compiled into Peare libraries; see [`src/writer/third_party/lzma/LICENSE.txt`](src/writer/third_party/lzma/LICENSE.txt), [`src/opener/compression/third_party/lzma/LICENSE.txt`](src/opener/compression/third_party/lzma/LICENSE.txt), and the [official SDK page](https://www.7-zip.org/sdk.html). |
| 7-Zip PPMd8 encoder | ZIP method 98 (PPMd-I) encoder | Public domain; see the source notice in [`Ppmd8Enc.c`](src/writer/third_party/lzma/Ppmd8Enc.c) and [upstream revision `d3296e39e1f1f92c7b41c0c3848e65640428d42d`](https://github.com/ip7z/7zip/blob/d3296e39e1f1f92c7b41c0c3848e65640428d42d/C/Ppmd8Enc.c). |
| Zstandard 1.5.7 | Zstandard frame compression and decompression for standalone `.zst` files and ZIPX | BSD-3-Clause; source is compiled into Peare modules; see [`src/opener/compression/third_party/zstd/LICENSE`](src/opener/compression/third_party/zstd/LICENSE) and the [upstream repository](https://github.com/facebook/zstd). |
| wimlib LZMS decoder | LZMS block decompression for Microsoft XBCOMPRESS | LGPL-2.1-or-later; source is compiled into `PeareOpener`; see [`src/opener/compression/third_party/wimlib/COPYING.LGPL`](src/opener/compression/third_party/wimlib/COPYING.LGPL), upstream revision `cd5e231c348c255ae5088873b5a66ee0eb96fa07`, and the [upstream repository](https://github.com/ebiggers/wimlib). |
| wimlib common Huffman routines | Shared decode-table helper used by the LZMS decoder | MIT; see the notices in `decompress_common.c` and [`src/opener/compression/third_party/wimlib/src/compress_common.c`](src/opener/compression/third_party/wimlib/src/compress_common.c). |
| WavPack | WavPack audio decompression for ZIP method 97 | BSD-3-Clause; source is compiled into `PeareOpener`; see [`src/opener/compression/third_party/wavpack/LICENSE.txt`](src/opener/compression/third_party/wavpack/LICENSE.txt); upstream revision `0931f1f3405d07b5f0511cf218bfbc6eb64d4c7d` and [repository](https://github.com/dbry/WavPack). |
| Mark Adler's `blast` 1.3 | PKWARE DCL Implode decompression for ZIP method 10 | zlib license; see the notice in [`blast.h`](src/opener/compression/third_party/blast/blast.h) and [upstream source](https://github.com/madler/zlib/tree/master/contrib/blast). |
| miniz | Streaming raw DEFLATE compression for ZIP Writer entries | MIT; see [`LICENSE`](src/writer/third_party/miniz/LICENSE); revision `2948eebafc5b9f1c69bebed80333e403a09bd62d` and [upstream source](https://github.com/richgel999/miniz). |
| XADMaster WinZip JPEG decoder | ZIP method 96 JPEG decompression and the shared arithmetic probability tables used by the native encoder | LGPL-2.1-or-later; source is compiled into `PeareOpener` and `PeareWriter`; revision `7cb9ee0abbb163f261e4cb74501e15067032319c`; see [`LICENSE`](src/opener/compression/third_party/winzipjpeg/LICENSE) and [upstream source](https://github.com/MacPaw/XADMaster/tree/master/WinZipJPEG). |
| packMP3 1.0g | ZIP method 94 MP3 decompression | LGPL-3.0-or-later; source is compiled into `PeareOpener`; revision `e61c11941552f2ffe6e219a847f441d9520d2e50`; see [`LICENSE`](src/opener/compression/third_party/packmp3/LICENSE) and the [upstream project](https://github.com/packjpg/packMP3). |
| NTFS-3G 2026.9.28 | Integrated `libntfs-3g` and `ntfsprogs` writer sources | GPL-2.0-or-later; see [`COPYING`](src/writer/third_party/ntfs-3g/COPYING) and [`CREDITS`](src/writer/third_party/ntfs-3g/CREDITS); [upstream source](https://github.com/tuxera/ntfs-3g) |
| FUSE-Lite headers | NTFS-3G compatibility headers | LGPL-2.0; see [`COPYING.LIB`](src/writer/third_party/ntfs-3g/COPYING.LIB) |

## Ported third-party code

These entries identify code that Peare ported or adapted from another project.
They are not clean-room implementations.

| Project or source | Ported code in Peare | Notice or license |
|---|---|---|
| CERF | Windows CE ROM and IMGFS parser port | [`src/opener/modules/wince/CERF-LICENSE.txt`](src/opener/modules/wince/CERF-LICENSE.txt) |
| DiscUtils | Btrfs, SquashFS, VDI, WIM, and XFS reader code; Peare adapted the source for its C++ codebase | MIT; see [`src/opener/fs/DISCUTILS-LICENSE.txt`](src/opener/fs/DISCUTILS-LICENSE.txt) and the [upstream repository](https://github.com/LTRData/DiscUtils). |

## Code, documentation, and data used as references

The entries in this table informed format work or supplied test data. Peare does
not include their implementation source, except where a row states that it uses
generated data or a fixture.

| Source | Use in Peare | Notice, license, or scope |
|---|---|---|
| Unicode Character Database 12.1.0 | Source for generated Unicode normalization and case-fold tables for Ext4 `CASEFOLD` | [`src/writer/third_party/lwext4/UNICODE-LICENSE.txt`](src/writer/third_party/lwext4/UNICODE-LICENSE.txt); source archive `https://www.unicode.org/Public/12.1.0/ucd/UCD.zip` |
| Unicode Character Database 8.0.0 | Source for the TIBX simple uppercase mapping | [`src/opener/fs/TIBX-UNICODE-LICENSE.txt`](src/opener/fs/TIBX-UNICODE-LICENSE.txt); [UnicodeData.txt](https://www.unicode.org/Public/8.0.0/ucd/UnicodeData.txt) |
| WMP WSZ format | Windows Media Player skin layout reference | [`src/decoder/resources/WSZ/NOTICE.txt`](src/decoder/resources/WSZ/NOTICE.txt), [`src/decoder/resources/WSZ/LICENSE-Apache-2.0.txt`](src/decoder/resources/WSZ/LICENSE-Apache-2.0.txt), and [format documentation](https://github.com/tdebaets/wmp-wsz-format) |
| go-filesystems/xar | XAR test fixtures only | [`tests/formats/xar/fixtures/LICENSE.txt`](tests/formats/xar/fixtures/LICENSE.txt); no source code is included |
| Spleen 2.2.0 font | WOFF2 ABI and GUI test fixture | BSD-3-Clause; see [`tests/abi_fonts/Spleen-LICENSE.txt`](tests/abi_fonts/Spleen-LICENSE.txt) |
| SSFN font fixture | SFN decoder test sample | MIT; see [`tests/abi_fonts/fixtures/SSFN-MIT-LICENSE.txt`](tests/abi_fonts/fixtures/SSFN-MIT-LICENSE.txt) |
| WPInternals | FFU format reference | <https://github.com/ReneLergner/WPinternals> |
| Linux JFS documentation | JFS on-disk format reference | IBM and Linux JFS maintainers |
| [OSTA UDF specifications](https://www.osta.org/specs/) | UDF | Specification reference only |
| [ECMA standards](https://ecma-international.org/publications-and-standards/standards/) | ECMA-119 ISO 9660 and ECMA-167 UDF | Specification reference only |
| [BZIP2 format specification](https://github.com/dsnet/compress/blob/master/doc/bzip2-format.pdf) | BZIP2 block and stream decoding | Format reference only |
| [XAR format reference](https://github.com/mackyle/xar/wiki/xarformat) | XAR header, table of contents, and heap | Format reference only |
| [DiscUtils](https://github.com/LTRData/DiscUtils) | Byte-store design reference | This reference is separate from the DiscUtils code listed in the ported-code table. |
| [wimlib WOF structures](https://github.com/ebiggers/wimlib/blob/master/include/wimlib/wof.h) | WOF WIM-provider on-disk layout | Format reference only; separate from the wimlib code listed above. |
| [dissect.archive](https://github.com/fox-it/dissect.archive) | TIBX format research and independent sample comparisons | Reference and test data only; no source code is included |
| [RFC 8017](https://www.rfc-editor.org/rfc/rfc8017), [RFC 7468](https://www.rfc-editor.org/rfc/rfc7468), [RFC 5958](https://www.rfc-editor.org/rfc/rfc5958) | RSA, PEM, and PKCS#8 decoding for TIBX private keys | Specification references only |
| [acronis-tib-reader](https://github.com/TreadingTheTiber/acronis-tib-reader) | TIB container and sector-map format research | Documentation and independent comparisons only; no source code is included |
| [acronis-tib](https://github.com/dennisss/acronis-tib) | TIB Mac and file-level format research | Reference only; no source code is included. |
| [tib-recover](https://github.com/elsbrock/tib-recover) | Legacy TIB file-level records and catalogs | Format reference only; no source code is included. |
| [Microsoft XBCOMPRESS sample](https://github.com/microsoft/Xbox-GDK-Samples/tree/main/Samples/Tools/xbcompress) | XBCOMPRESS header and independent test samples | Sample/reference only; no source code is included |
| [pymszip](https://github.com/frereit/pymszip) | MSZIP stream framing | Reference only; no source code is included |
| [WinCE Decompressor](https://github.com/KodaSec/wince-decompr) | Windows CE compression | Format reference only |
| [Libarchive RAR reader](https://github.com/libarchive/libarchive/blob/master/libarchive/archive_read_support_format_rar.c) | RAR4 algorithm | Reference only; no Libarchive source is included |
| [Lhasa](https://github.com/fragglet/lhasa) | LHA/LZH algorithms | Reference only; no Lhasa source is included |
| [OS/2 object module format](https://www.edm2.com/index.php/IBM_OS/2_16/32-bit_Object_Module_Format_) | NE, LE, and LX executable structures | Format reference only |
| [Resources and Decompiling Them](https://www.edm2.com/index.php/Resources_and_Decompiling_Them) | OS/2 and Windows resources | Format reference only |
| [IBM OS/2 technical library](https://bitsavers.trailing-edge.com/pdf/ibm/pc/os2/) | OS/2 driver and system formats | Documentation reference only |
| [OS/2 Museum](https://www.os2museum.com/) | Windows 1.x and 2.x formats | Historical reference only |
| [BetaArchive format discussion](https://www.betaarchive.com/forum/viewtopic.php?t=33486) | Historical format research | Discussion reference only |

Libarchive is a read-only RAR4 reference. Peare does not include Libarchive code.

## Trademark notice

Peare uses format names and trademarks to identify supported data. Their owners
retain those names and marks. Peare is independent and does not claim endorsement.
See [COPYRIGHT](COPYRIGHT.md) for the owner list.
