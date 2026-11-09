# Supported Formats

This document lists format variants supported by Peare. It separates formats
opened by the Reader and Writer from payloads interpreted by the Decoder.

`Read` means that PeareOpener can open the listed variant. `Write` means that
PeareWriter can create or update it. A `Yes` value applies only to the variant
and operations described in this document. It does not mean that every feature
of the format is supported.

Resources inside a file are not separate readable or writable formats. The
Opener returns resource payloads. PeareDecoder interprets supported payloads.
Writer support for a parent format does not imply support for editing its
embedded resources.

See [Archive reader limits](formats/READER_LIMITATIONS.md) for unsupported
archive variants.

See [ZIP package compatibility](formats/ZIP_PACKAGE_COMPATIBILITY.md) for
package-specific limits on ZIP-based files.

## Reader and Writer

### Filesystems

| Format | Description | Read | Write |
|---|---|---:|---:|
| FAT file system | FAT12, FAT16, and FAT32 | Yes | Yes |
| exFAT file system | exFAT 1.0 | Yes | Yes |
| NTFS file system | NTFS volumes with 512-byte or 4096-byte logical sectors | Yes | Yes |
| Ext2 file system | Ext2 volumes | Yes | Yes |
| Ext3 file system | Ext3 volumes | Yes | Yes |
| Ext4 file system | Ext4 volumes with supported feature flags | Yes | Yes |
| XFS file system | XFS versions 4 and 5 | Yes | No |
| JFS file system | JFS1 versions 1 and 2 | Yes | No |
| HPFS file system | OS/2 HPFS | Yes | No |
| Btrfs file system | Single-device volumes | Yes | No |
| SquashFS file system | SquashFS version 4 | Yes | No |
| CramFS file system | Little-endian and big-endian images, 512-byte boot padding, and validated 64-byte U-Boot prefixes | Yes | No |
| HFS+ / HFSX file system | HFS+ and HFSX | Yes | No |
| UDF file system | UDF Type 1 partitions | Yes | No |
| UFS1 file system | Traditional UFS1 layout, including Solaris/SPARC big-endian volumes | Yes | No |
| UFS2 / FFS2 file system | BSD disklabel and GPT volumes, including FreeBSD UFS2 | Yes | No |
| Linux swap | Versions 1 and 2 | Yes | No |
| Linux LVM2 | Single-PV linear and striped volumes | Yes | No |
| Linux MD RAID | RAID1, metadata versions 0.90 and 1.x | Yes | No |
| Windows Dynamic Disk | Concatenated and striped LDM volumes | Yes | No |
| PlayStation 2 ROMDIR / IOPRP | BIOS and reboot-image file systems | Yes | No |

### Virtual Hard Disks

| Format | Description | Read | Write |
|---|---|---:|---:|
| VMDK monolithicSparse | Sparse extent, versions 1 and 2 | Yes | Yes |
| VMDK twoGbMaxExtentSparse | Split sparse extents, versions 1 and 2 | Yes | Yes |
| VMDK monolithicFlat | Monolithic flat extent | Yes | Yes |
| VMDK twoGbMaxExtentFlat | Split flat extents | Yes | Yes |
| VMDK streamOptimized | Version 3 compressed extent | Yes | Yes |
| VHD fixed | Fixed disk | Yes | Yes |
| VHD dynamic | Dynamic disk | Yes | Yes |
| VHD differencing | Differencing disk | No | No |
| VHDX fixed | Fixed disk with 512-byte or 4Kn logical sectors | Yes | Yes |
| VHDX dynamic | Dynamic disk with 512-byte or 4Kn logical sectors | Yes | Yes |
| VHDX differencing / dirty log | Differencing disks and dirty-log recovery | No | No |
| VirtualBox VDI | Fixed and dynamic disks | Yes | No |
| QEMU QCOW | Version 1 | Yes | No |
| QEMU QCOW2 | Versions 2 and 3 | Yes | No |
| Parallels HDD | Expandable sparse disk images | Yes | No |

### Backups and Deployment Images

| Format | Description | Read | Write |
|---|---|---:|---:|
| Microsoft Tape Format | MTF and NTBackup BKF | Yes | No |
| Microsoft WIM | WIM images and split WIM sets (`.swm`) | Yes | No |
| Microsoft FFU | Windows Phone and desktop FFU, versions 1 and 2 | Yes | No |
| Microsoft SDI | System Deployment Image sections | Yes | No |

### Disk Images

| Format | Description | Read | Write |
|---|---|---:|---:|
| Android sparse image | Version 1.0 RAW, FILL, DONT_CARE, and CRC32 chunks | Yes | No |
| Android dynamic-partition super image | LP metadata versions 10.0-10.2; one backing block device; linear and zero extents | Yes | No |
| Amiga ADF floppy image | 880 KiB OFS and FFS volumes, including international and directory-cache variants | Yes | No |
| Amiga ADZ compressed floppy image | Single-member GZIP stream containing an 880 KiB ADF with a valid Amiga DOS file system | Yes | No |
| Raw floppy IMG/IMA/VFD | Standard raw floppy images | Yes | Yes |
| WinImage IMZ | Unencrypted ZIP-compatible image with one standard-capacity `.ima` or `.img` member | Yes | No |
| OS/2 XDF floppy DSK | 3.5-inch, 1.84 MiB XDF | Yes | Yes |
| Raw MBR/GPT disk | MBR and GPT partitioned disks | Yes | Yes |
| Apple Partition Map | Partitioned disk images | Yes | No |
| ISO 9660 | ISO 9660 with Joliet, standard and raw Mode 2 images | Yes | No |
| CDRWIN BIN/CUE | Multi-track optical-disc images | Yes | No |
| Alcohol MDF/MDS | MDF images with MDS descriptors | Yes | No |
| Apple DMG / UDIF | UDIF disk images | Yes | No |
| Xen XVA | Xen virtual appliances | Yes | No |

### Archives and Packages

| Format | Description | Read | Write |
|---|---|---:|---:|
| ZIP | Store, Shrink, Implode, Deflate, Deflate64, BZip2, LZMA, DCL Implode, ZIPX methods, and ZIP64; Writer methods are tracked in ZIP Writer Methods; `.zapXX` files use the same ZIP reader | Yes | Yes |
| ZIPX | WinZip extensions: XZ, Zstandard, PPMd, WavPack, MP3, and JPEG compression; Writer method coverage is tracked in ZIP Writer Methods | Yes | Yes |
| 7-Zip (7z) | 7z archives | Yes | No |
| NSIS | Nullsoft Scriptable Install System installers | Yes | No |
| RAR4 | RAR4 archives | Yes | No |
| RAR5 | RAR5 archive format, algorithm version 0 | Yes | No |
| RAR7 | RAR5 archive format, algorithm version 1 | Yes | No |
| Nintendo Switch NUP | NUP v1 base, update, and add-on trees; raw and Zstandard file payloads | Yes | No |
| Java JAR / WAR / EAR / AAR | Java and Android ZIP archives | Yes | Yes |
| Android APK | ZIP package contents and v2/v3/v3.1 signer leaf-certificate metadata | Yes | Yes |
| Android APKS / AAB | ZIP package contents | Yes | Yes |
| EPUB | EPUB ZIP container | Yes | Yes |
| Office Open XML | DOCX, XLSX, and PPTX packages | Yes | Yes |
| OpenDocument | ODT, ODS, and ODP packages | Yes | Yes |
| Apple IPA / IPSW | iOS ZIP containers | Yes | Yes |
| Firefox XPI | Browser extension ZIP package | Yes | Yes |
| NuGet | NUPKG and SNUPKG packages | Yes | Yes |
| Python wheel | WHL package | Yes | Yes |
| Visual Studio extension | VSIX package | Yes | Yes |
| Comic book ZIP | CBZ archive | Yes | Yes |
| Google Earth KMZ | KMZ archive | Yes | Yes |
| TAR | V7 and USTAR | Yes | Yes |
| TAR.GZ / TGZ | USTAR archive in an RFC 1952 GZIP stream | Yes | Yes |
| TAR.BZ2 / TBZ / TBZ2 | USTAR archive in a BZIP2 stream; compression levels 1-9 | Yes | Yes |
| OS/2 WarpIN (WPI) | Raw revision 3 and 4 packages; stored and bzip2-compressed payloads. Self-extracting EXE packages are not included. | Yes | No |
| RPM | RPM v3/v4 packages with CPIO payloads | Yes | No |
| Microsoft Cabinet | Uncompressed, MSZIP, LZX (window sizes 15 to 21), and Quantum (window sizes 10 to 21) | Yes | No |
| Microsoft Compiled HTML Help (CHM) | ITSF versions 2 and 3 with LZX-compressed content | Yes | No |
| Electron ASAR | Packed and unpacked members | Yes | No |
| GZIP (`.gz`, `.gzip`) | RFC 1952 streams, including concatenated members; the Writer creates one member per selected input file. The Reader also opens `.tgz` streams. | Yes | Yes |
| XZ (`.xz`, `.txz`) | LZMA2 streams, concatenation, stream padding, and no check, CRC32, CRC64, or SHA-256 checks | Yes | Yes |
| LZMA-alone (`.lzma`) | LZMA stream with declared unpacked size; Writer levels 1-9 | Yes | Yes |
| OS/2 PACK | IBM and Microsoft PACK version 1 | Yes | No |
| OS/2 PACK2 | FTCOMP `fT19` archives only; `fT21` is not supported | Yes | No |
| BZIP2 (`.bz2`, `.bzip2`, `.tbz`, `.tbz2`) | Block levels 1-9; multi-block and concatenated streams; the Writer creates one member per selected input file | Yes | Yes |
| Unix compress (`.Z`, `.taz`) | LZW block and non-block modes, code widths 9-16 bits | Yes | Yes |
| Lzip (`.lz`, `.tlz`) | Version 1 members, including concatenated members | Yes | No |
| Zstandard (`.zst`) | Standard dictionary-free frames, including concatenated and checksummed frames; Writer levels -5 through 22 | Yes | Yes |
| Unix ar | GNU and BSD archives with short and extended member names; thin archives are not supported | Yes | Yes |
| ARJ | Methods 0-4, legacy, GOST40, GOST256, and GOST256L password encryption, and split volumes | Yes | No |
| XAR | XAR archives and Apple flat installer packages | Yes | No |
| CPIO | SVR4 `newc` and CRC, POSIX `odc`, afio large ASCII, and old binary little- and big-endian archives | Yes | Yes |
| Debian package | `.deb` and `.udeb`, version 2.0; raw or gzip/xz/zstd control TAR, plus raw/gzip/xz/zstd/bzip2/lzma data TAR | Yes | No |
| LHA / LZH | LHA and LZH archives | Yes | No |
| Doom WAD | IWAD and PWAD archives | Yes | Yes |
| Quake WAD2 | WAD2 archives | Yes | No |
| Half-Life WAD3 | WAD3 archives | Yes | No |
| APPX / MSIX | Windows application ZIP packages | Yes | Yes |
| Encrypted APPX / MSIX | EAPPX and EMSIX envelopes | Yes | No |
| APPXBUNDLE | Windows application ZIP bundle | Yes | Yes |
| Encrypted APPXBUNDLE | EAPPXBUNDLE and EMSIXBUNDLE envelopes | Yes | No |
| XAP | Silverlight and Windows Phone ZIP packages | Yes | Yes |
| PlayReady XAP envelope | PlayReady-protected XAP packages | Yes | No |

### Encodings

| Format | Description | Read | Write |
|---|---|---:|---:|
| UUencode | Classic and GNU Base64 single-file encoding | Yes | No |

### Binaries

| Format | Description | Read | Write |
|---|---|---:|---:|
| DOS MZ executable | DOS MZ executables | Yes | No |
| Windows NE executable | 16-bit Windows NE | Yes | No |
| OS/2 NE executable | OS/2 NE | Yes | No |
| Windows PE executable | 32-bit and 64-bit PE | Yes | No |
| OS/2 LE executable | OS/2 LE | Yes | No |
| OS/2 LX executable | OS/2 LX | Yes | No |
| Xbox XBE executable | Original Xbox XBE | Yes | No |
| Xbox 360 XEX executable | XEX1 and XEX2 | Yes | No |
| DirectX Shader Container (DXBC) | Version 1 containers; validated chunk table and lazy raw chunk access | Yes | No |
| Xbox 360 database (XDBF/GPD) | Version 1.0 records, title string, and image records | Yes | No |
| XNA / MonoGame XNB | Versions 4 and 5; uncompressed, LZX, and raw LZ4; metadata and serialized root asset, with Color Texture2D preview | Yes | No |
| .NET RESX | .NET XML resource files | Yes | No |
| Windows Installer CFB | MSI, MSM, MSP, MST, PCP, and CUB files | Yes | No |
| Outlook MSG | Outlook MSG files | Yes | No |
| Apple resource fork | Generic Apple resource records, including data-fork resource maps | Yes | No |
| OS/2 FEALIST | OS/2 extended-attribute files | Yes | No |
| Microsoft Compress | SZDD variants A and B | Yes | No |
| Microsoft KWAJ compression | Methods 0, 1, 2, 3, and 4 | Yes | No |
| Microsoft XBCOMPRESS | LZMS and MSZIP | Yes | No |
| PlayStation 3 PUP | PUP version 1 | Yes | No |
| Siemens ProSave IMG | Siemens HMI IMG | Yes | No |
| Siemens ProSave FWF | Siemens HMI FWF | Yes | No |
| Siemens FSF | Siemens FSF | Yes | No |
| Siemens ADONIS / SMART_OS | ADONIS and SMART_OS images | Yes | No |
| Intel HEX | Data records, checksums, and segment or linear addresses | Yes | No |
| Windows CE ROM / IMGFS | Windows CE ROM and IMGFS variants | Yes | No |
| Windows Registry hive | Windows Registry hive files | Yes | No |
| Windows BCD | Windows Boot Configuration Data | Yes | No |

## Decoder

The Decoder interprets raw files and payloads from Reader formats. The tables
list the raw input formats that the Decoder recognizes. For embedded resource
types, see [Resource compatibility](formats/RESOURCE_COMPATIBILITY.md).

### Images

| Format | Description |
|---|---|
| PNG | Portable Network Graphics |
| JPEG | JPEG image |
| PCX / PCC | ZSoft PCX and PCC images |
| GIF | GIF87a and GIF89a |
| Windows BMP, CORE DIB | 12-byte `BITMAPCOREHEADER`, 1-, 2-, 4-, 8-, and 24-bit pixels |
| Windows BMP, INFO DIB | `BITMAPINFOHEADER` and later headers, 1-, 2-, 4-, 8-, 16-, 24-, and 32-bit pixels, `BI_RGB`, `BI_RLE4`, `BI_RLE8`, and `BI_BITFIELDS` |
| OS/2 BMP 1.x | 1-, 2-, 4-, and 8-bit indexed images |
| OS/2 BMP 2.x | `BITMAPINFOHEADER2`, uncompressed, monochrome Huffman 1D, RLE4, RLE8, and RLE24 |
| OS/2 bitmap array | Array containers with one or more bitmap images |
| TIFF | Tagged Image File Format |
| ICO | Windows icon image |
| CUR | Windows cursor image |
| WMF | Windows Metafile image |
| EMF | Enhanced Metafile image |
| Doom patch lump (`WAD_DOOM_PATCH`) | Column-post picture with the palette from `PLAYPAL` |
| Doom sprite lump (`WAD_DOOM_SPRITE`) | Column-post sprite with the palette from `PLAYPAL` |
| Doom wall texture definitions (`TEXTURE1`, `TEXTURE2`) | Composite textures built from `WAD_TEXTUREX`, `PNAMES`, and Doom patch lumps |
| Siemens / Microwindows `MWIMAGEHDR` | Raw indexed and true-color images, 1, 2, 4, 8, 16, 24, and 32 bpp |
| Xbox `XBE_LOGO_RLE` | Raw run-length encoded Xbox logo image |

### Structured data

| Format | Description |
|---|---|
| Android Binary XML (AXML) | Compiled Android XML resources, including `AndroidManifest.xml` |

### Fonts

| Format | Description |
|---|---|
| Windows FNT | Versions 1.0, 2.0, and 3.0 |
| OS/2 FNT | OS/2 bitmap and outline fonts |
| BDF / Sun AFB | Bitmap Distribution Format and Sun AFB ASCII fonts |
| Berkeley / Sun VFONT | Berkeley and Sun VFONT bitmap fonts |
| Sun X11/NeWS binary bitmap font | Legacy and extended CJK `.fb` tables |
| DECwindows Font (DWF), legacy | ULTRIX/UWS 75 dpi and 100 dpi compiled bitmap fonts |
| DECwindows Font (DWF), MIT X11 | MIT X11 compiled bitmap font variant |
| DECwindows Font (DWF), CompX10 | DEC CompX10 compiled bitmap font variant |
| ByteMap Font (BMF) | Versions 1.1 and 1.2 |
| PC Screen Font (PSF) | PSF1 and PSF2 |
| Portable Compiled Format (PCF) | PCF bitmap fonts |
| X11 Server Normal Format (SNF) | Version 4, little-endian |
| ZX Spectrum FZX | Proportional FZX bitmap fonts |
| TeX Packed Font (PK) | Short, extended, and long packet forms |
| Adobe Font Metrics (AFM) | AFM metrics; outline rendering is not supported |
| Printer Font Metrics (PFM) | Binary font metrics; outline rendering is not supported |
| PostScript Type 1 | ASCII PFA and binary PFB |
| PostScript Type 3 | Type 3 fonts with supported path operators |
| SVG Font | SVG 1.1 embedded fonts |
| Scalable Screen Font (SFN) | SFN2 fonts and SFNC collections |
| TrueType (TTF) | TrueType fonts and TTC collections |
| OpenType (OTF) | CFF/CFF2 OpenType fonts and TTC collections |
| Web Open Font Format (WOFF) | WOFF 1.0 |
| Web Open Font Format 2 (WOFF2) | WOFF2 |
| Embedded OpenType (EOT) | EOT 1.0, 2.0.1, and 2.0.2 |
| AutoCAD SHP / SHX | SHP source fonts and compiled SHX fonts |
| Amiga bitmap font | FCH and supported TFCH font data |
| Amiga AnimFont | IFF FORM ANIM brush fonts |
| Apple resource-fork font records | sfnt, NFNT/FONT bitmap strikes, and FOND metadata |
