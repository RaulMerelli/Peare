# Third-party notices

Peare is distributed under the GNU Affero General Public License, version 3.
See [LICENSE](LICENSE) for its terms.

## Code and format references

| Project or source | Use in Peare | Notice or license |
|---|---|---|
| Qt | GUI toolkit and platform services | The license depends on each Qt module and build. Review the notices supplied with the Qt distribution and the [official licensing terms](https://www.qt.io/development/qt-licensing). |
| FatFs R0.16 | FAT file-system writer | [`src/writer/third_party/fatfs/LICENSE.txt`](src/writer/third_party/fatfs/LICENSE.txt) |
| lwext4 | Ext2/Ext3 formatter and file-system writer; only BSD-3-Clause files are included | [`src/writer/third_party/lwext4/LICENSE-BSD-3-Clause.txt`](src/writer/third_party/lwext4/LICENSE-BSD-3-Clause.txt); source revision `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5` |
| Unicode Character Database 12.1.0 | Generated Unicode normalization and case-fold tables for Ext4 `CASEFOLD` | [`src/writer/third_party/lwext4/UNICODE-LICENSE.txt`](src/writer/third_party/lwext4/UNICODE-LICENSE.txt); source archive `https://www.unicode.org/Public/12.1.0/ucd/UCD.zip` |
| CERF | Windows CE ROM and IMGFS parser port | [`src/opener/modules/wince/CERF-LICENSE.txt`](src/opener/modules/wince/CERF-LICENSE.txt) |
| WMP WSZ format | WSZ format reference | [`src/decoder/resources/WSZ/NOTICE.txt`](src/decoder/resources/WSZ/NOTICE.txt) and [`LICENSE-Apache-2.0.txt`](src/decoder/resources/WSZ/LICENSE-Apache-2.0.txt) |
| Google WOFF2 decoder | WOFF2 font decompression | [`src/decoder/font/third_party/woff2/LICENSE`](src/decoder/font/third_party/woff2/LICENSE); revision `7b67e12355fe7da148db5c19d4261f0a498a9537` |
| Google Brotli decoder | WOFF2 Brotli decompression | [`src/decoder/font/third_party/brotli/LICENSE`](src/decoder/font/third_party/brotli/LICENSE); revision `d89c4b1c52e25ddafe047bc78bbee5c1e24b1809` |
| libeot 0.01 | EOT MicroType Express decompression and CTF reconstruction | [`src/decoder/font/third_party/libeot/LICENSE`](src/decoder/font/third_party/libeot/LICENSE), [`PATENTS`](src/decoder/font/third_party/libeot/PATENTS); revision `0407abddc581d32e9871ee41535183ee1d924d85` |
| bzip2 1.0.8 | Legacy 512-value randomization table for pre-1.0 BZIP2 blocks | [`src/opener/compression/third_party/bzip2/LICENSE`](src/opener/compression/third_party/bzip2/LICENSE); [source archive](https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz) |
| LZMA SDK 26.05 | LZMA, LZMA2, PPMd7, Delta, and BCJ decoders used by the 7z reader | Public domain. The decoder source is integrated into `PeareOpener`; see [`src/opener/compression/third_party/lzma/LICENSE.txt`](src/opener/compression/third_party/lzma/LICENSE.txt) and the [official SDK page](https://www.7-zip.org/sdk.html). |
| NTFS-3G 2026.9.28 | Integrated `libntfs-3g` and `ntfsprogs` writer sources | GPL-2.0-or-later; see [`COPYING`](src/writer/third_party/ntfs-3g/COPYING) and [`CREDITS`](src/writer/third_party/ntfs-3g/CREDITS); [upstream source](https://github.com/tuxera/ntfs-3g) |
| FUSE-Lite headers | NTFS-3G compatibility headers | LGPL-2.0; see [`COPYING.LIB`](src/writer/third_party/ntfs-3g/COPYING.LIB) |
| Spleen 2.2.0 font | License-safe WOFF2 ABI and GUI fixture | BSD-3-Clause; see [`tests/abi_fonts/Spleen-LICENSE.txt`](tests/abi_fonts/Spleen-LICENSE.txt) |
| SSFN font fixture | SFN decoder test sample | MIT; see [`tests/abi_fonts/fixtures/SSFN-MIT-LICENSE.txt`](tests/abi_fonts/fixtures/SSFN-MIT-LICENSE.txt) |
| DiscUtils | Ported or adapted code in the Btrfs, SquashFS, VDI, WIM, and XFS readers, plus byte-store design references | MIT; see [`src/opener/fs/DISCUTILS-LICENSE.txt`](src/opener/fs/DISCUTILS-LICENSE.txt) and the [upstream repository](https://github.com/LTRData/DiscUtils). |
| WPInternals | FFU format reference | <https://github.com/ReneLergner/WPinternals> |
| Linux JFS documentation | JFS on-disk format reference | IBM and Linux JFS maintainers |

Some Peare readers are ports or adaptations of DiscUtils implementations. They
are not clean-room implementations. Libarchive is a read-only RAR4 reference;
Peare does not include Libarchive code.

## Format and platform references

| Source | Format or topic |
|---|---|
| [OSTA UDF specifications](https://www.osta.org/specs/) | UDF |
| [ECMA standards](https://ecma-international.org/publications-and-standards/standards/) | ECMA-119 ISO 9660 and ECMA-167 UDF |
| [WMP WSZ format](https://github.com/tdebaets/wmp-wsz-format) | Windows Media Player skin layout |
| [BZIP2 format specification](https://github.com/dsnet/compress/blob/master/doc/bzip2-format.pdf) | BZIP2 block and stream decoding |
| [CERF](https://github.com/gweslab/cerf) | Windows CE ROM and IMGFS |
| [WinCE Decompressor](https://github.com/KodaSec/wince-decompr) | Windows CE compression |
| [Libarchive RAR reader](https://github.com/libarchive/libarchive/blob/master/libarchive/archive_read_support_format_rar.c) | RAR4 algorithm reference only; no Libarchive source is included |
| [Lhasa](https://github.com/fragglet/lhasa) | LHA/LZH algorithm reference only; no Lhasa source is included |
| [OS/2 object module format](https://www.edm2.com/index.php/IBM_OS/2_16/32-bit_Object_Module_Format_) | NE, LE, and LX executable structures |
| [Resources and Decompiling Them](https://www.edm2.com/index.php/Resources_and_Decompiling_Them) | OS/2 and Windows resources |
| [IBM OS/2 technical library](https://bitsavers.trailing-edge.com/pdf/ibm/pc/os2/) | OS/2 driver and system formats |
| [OS/2 Museum](https://www.os2museum.com/) | Windows 1.x and 2.x references |
| [BetaArchive format discussion](https://www.betaarchive.com/forum/viewtopic.php?t=33486) | Historical format research |

## Trademark notice

Peare uses format names and trademarks to identify supported data. Their owners
retain those names and marks. Peare is independent and does not claim endorsement.
See [COPYRIGHT](COPYRIGHT.md) for the owner list.
