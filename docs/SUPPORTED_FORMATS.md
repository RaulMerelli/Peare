# Supported Formats

This document lists format variants supported by Peare. It separates formats
opened by the Reader and Writer from payloads interpreted by the Decoder.

`Extension` lists common file-name suffixes. A dash means that the format has
no fixed suffix, or that it normally exists inside another file or volume.

`Read` means that PeareOpener can open the listed variant. `Write: Yes` means
that PeareWriter can create the variant from zero and perform every applicable
CUD operation on existing targets. It does not mean that every other feature
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

| Extension | Format | Description | Read | Write |
|---|---|---|---:|---:|
| `.vmdk` | monolithicSparse | Sparse extent, versions 1 and 2 | Yes | Yes |
| `.vmdk` | twoGbMaxExtentSparse | Split sparse extents, versions 1 and 2 | Yes | Yes |
| `.vmdk` | monolithicFlat | Monolithic flat extent | Yes | Yes |
| `.vmdk` | twoGbMaxExtentFlat | Split flat extents | Yes | Yes |
| `.vmdk` | streamOptimized | Version 3 compressed extent | Yes | Yes |
| `.vhd` | Fixed | Fixed disk | Yes | Yes |
| `.vhd` | Dynamic | Dynamic disk | Yes | Yes |
| `.vhd` | Differencing | Differencing disk | No | No |
| `.vhdx` | Fixed | Fixed disk with 512-byte or 4Kn logical sectors | Yes | Yes |
| `.vhdx` | Dynamic | Dynamic disk with 512-byte or 4Kn logical sectors | Yes | Yes |
| `.vhdx` | Differencing / dirty log | Differencing disks and dirty-log recovery | No | No |
| `.vdi` | VirtualBox disk image | Fixed and dynamic disks | Yes | No |
| `.qcow` | QEMU disk image | Version 1 | Yes | No |
| `.qcow2` | QEMU disk image | Versions 2 and 3 | Yes | No |
| `.hdd` | Parallels disk image | Expandable sparse disk images | Yes | No |

### Backups and Deployment Images

| Extension | Format | Description | Read | Write |
|---|---|---|---:|---:|
| `.mtf`, `.bkf` | Microsoft tape backup | MTF and NTBackup BKF | Yes | No |
| `.tib` | Acronis True Image Backup | Supported disk-image and file-level variants; see [Acronis Reader](formats/ACRONIS_READER.md) for limits | Yes | No |
| `.tibx` | Acronis TIBX backup | Verified archive variants; see [Acronis Reader](formats/ACRONIS_READER.md) for limits | Yes | No |
| `.wim` | Windows imaging format | Single-part images with Store, XPRESS, or LZX compression | Yes | Yes |
| `.swm` | Split Windows image set | Split image sets | Yes | No |
| `.ffu` | Firmware update image | Windows Phone and desktop versions 1 and 2 | Yes | No |
| `.sdi` | System deployment image | System Deployment Image sections | Yes | No |

### Disk Images

| Extension | Format | Description | Read | Write |
|---|---|---|---:|---:|
| `.img` | Android sparse image | Version 1.0 RAW, FILL, DONT_CARE, and CRC32 chunks | Yes | No |
| `.img` | Android dynamic-partition super image | LP metadata versions 10.0-10.2; one backing block device; linear and zero extents | Yes | No |
| `.adf` | Amiga floppy image | 880 KiB OFS and FFS volumes, including international and directory-cache variants | Yes | No |
| `.adz` | Amiga compressed floppy image | Single-member GZIP stream containing an 880 KiB floppy image with a valid Amiga DOS file system | Yes | No |
| `.img`, `.ima`, `.vfd` | Raw floppy image | Standard raw floppy images | Yes | Yes |
| `.imz` | WinImage floppy image | Unencrypted ZIP-compatible image with one standard-capacity `.ima` or `.img` member | Yes | No |
| `.dsk` | OS/2 XDF floppy image | 3.5-inch, 1.84 MiB XDF | Yes | Yes |
| `.img`, `.ima`, `.raw` | Raw MBR/GPT disk | MBR and GPT partitioned disks | Yes | Yes |
| varies | Apple Partition Map | Partitioned disk images | Yes | No |
| `.iso` | Optical-disc image | Reader: Joliet, standard, and raw Mode 2 images. Writer: create and update 2048-byte-sector Joliet images. | Yes | Yes |
| `.bin`, `.cue` | CDRWIN multi-track image | Multi-track optical-disc images | Yes | No |
| `.mdf`, `.mds` | Alcohol disc image | MDF images with MDS descriptors | Yes | No |
| `.dmg` | Apple disk image | UDIF disk images | Yes | No |
| `.xva` | Xen virtual appliance | Xen virtual appliances | Yes | No |

### Archives and Packages

| Extension | Format | Description | Read | Write |
|---|---|---|---:|---:|
| `.zip` | General-purpose archive | Store, Shrink, Implode, Deflate, Deflate64, BZip2, LZMA, DCL Implode, ZIPX methods, and ZIP64; Writer methods are tracked in [ZIP package compatibility](formats/ZIP_PACKAGE_COMPATIBILITY.md) | Yes | Yes |
| `.zipx` | WinZip extension archive | XZ, Zstandard, PPMd, WavPack, MP3, and JPEG compression; Writer method coverage is tracked in [ZIP package compatibility](formats/ZIP_PACKAGE_COMPATIBILITY.md) | Yes | Yes |
| `.7z` | 7-Zip | 7z archives | Yes | Yes |
| `.exe` | NSIS | Nullsoft Scriptable Install System installers | Yes | No |
| `.rar` | Version 4 archive | RAR 4.x format | Yes | Yes |
| `.rar` | Version 5 archive | RAR 5 format, algorithm version 0 | Yes | Yes |
| `.rar` | Version 7 archive | RAR 5 format, algorithm version 1 | Yes | Yes |
| `.nup` | Nintendo Switch content package | Base, update, and add-on trees; raw and Zstandard file payloads | Yes | No |
| `.apk` | Android package | ZIP package contents and v2/v3/v3.1 signer leaf-certificate metadata | Yes | Yes |
| `.tar` | Uncompressed archive | V7 and USTAR | Yes | Yes |
| `.tar.gz`, `.tgz` | GZIP-compressed archive | USTAR archive in an RFC 1952 stream | Yes | Yes |
| `.tar.bz2`, `.tbz`, `.tbz2` | BZIP2-compressed archive | USTAR archive in a BZIP2 stream; compression levels 1-9 | Yes | Yes |
| `.tar.zst`, `.tzst`, `.tar.zstd` | Zstandard-compressed archive | USTAR archive in a Zstandard frame; compression levels -5 to 22 | Yes | Yes |
| `.tar.z`, `.taz` | Unix-compress archive | USTAR archive in a Unix compress stream; block and non-block modes, code widths 9-16 bits | Yes | Yes |
| `.tar.xz`, `.txz` | XZ-compressed archive | USTAR archive in an XZ stream; no check, CRC32, CRC64, or SHA-256 checks | Yes | Yes |
| `.tar.lzma` | LZMA-compressed archive | USTAR archive in an LZMA-alone stream; compression levels 1-9 | Yes | Yes |
| `.wpi` | OS/2 WarpIN | Raw revision 3 and 4 packages; stored and bzip2-compressed payloads. Self-extracting EXE packages are not included. | Yes | No |
| `.rpm` | Red Hat package | Versions 3 and 4 with CPIO payloads | Yes | Yes |
| `.cab` | Microsoft Cabinet | Uncompressed, MSZIP, LZX (window sizes 15 to 21), and Quantum (window sizes 10 to 21) | Yes | No |
| `.chm` | Microsoft Compiled HTML Help | ITSF versions 2 and 3 with LZX-compressed content | Yes | No |
| `.asar` | Electron archive | Packed and unpacked members | Yes | No |
| `.gz`, `.gzip`, `.tgz` | GZIP | RFC 1952 streams, including concatenated members; the Writer creates one member per selected input file. The Reader also opens `.tgz` streams. | Yes | Yes |
| `.xz`, `.txz` | XZ | LZMA2 streams, concatenation, stream padding, and no check, CRC32, CRC64, or SHA-256 checks | Yes | Yes |
| `.lzma` | LZMA-alone | LZMA stream with declared unpacked size; Writer levels 1-9 | Yes | Yes |
| varies | OS/2 PACK | IBM and Microsoft PACK version 1 | Yes | No |
| varies | OS/2 PACK2 | FTCOMP `fT19` archives only; `fT21` is not supported | Yes | No |
| `.bz2`, `.bzip2`, `.tbz`, `.tbz2` | Block-sorted compression stream | Levels 1-9; multi-block and concatenated streams; the Writer creates one member per selected input file | Yes | Yes |
| `.z` | Unix compress | LZW block and non-block modes, code widths 9-16 bits; one input file per stream | Yes | Yes |
| `.lz`, `.tlz` | Lzip | Version 1 members, including concatenated members; Writer levels 0-9 | Yes | Yes |
| `.zst` | Zstandard frame | Standard dictionary-free frames, including concatenated and checksummed frames; Writer levels -5 through 22 | Yes | Yes |
| `.a` | Unix ar | GNU and BSD archives with short and extended member names; thin archives are not supported | Yes | Yes |
| `.arj` | Compressed archive | Methods 0-4, legacy, GOST40, GOST256, and GOST256L password encryption, and split volumes | Yes | No |
| `.xar` | Extensible archive | Version 1 with stored, zlib, or bzip2 file data | Yes | Yes |
| `.pkg` | Apple flat installer package | XAR-based package layout and package metadata | Yes | No |
| `.cpio` | Copy-in/copy-out archive | SVR4 `newc` and CRC, POSIX `odc`, afio large ASCII, and old binary little- and big-endian archives | Yes | Yes |
| `.deb`, `.udeb` | Debian package | Version 2.0; raw or gzip/xz/zstd control TAR, plus raw/gzip/xz/zstd/bzip2/lzma data TAR | Yes | Yes |
| `.lha`, `.lzh` | LHA / LZH | LHA and LZH archives | Yes | No |
| `.wad` | Doom resource archive | IWAD and PWAD variants | Yes | Yes |
| `.wad` | Quake resource archive | WAD2 variant | Yes | No |
| `.wad` | Half-Life resource archive | WAD3 variant | Yes | No |
| `.appx`, `.msix` | Windows application package | ZIP package with a validated application manifest | Yes | Yes |
| `.eappx`, `.emsix` | Encrypted APPX / MSIX | EAPPX and EMSIX envelopes | Yes | No |
| `.appxbundle` | Windows application bundle | ZIP bundle with a validated bundle manifest | Yes | Yes |
| `.eappxbundle`, `.emsixbundle` | Encrypted APPXBUNDLE | EAPPXBUNDLE and EMSIXBUNDLE envelopes | Yes | No |
| `.xap` | Silverlight / Windows Phone package | ZIP package with a validated application manifest | Yes | Yes |
| `.xap` | PlayReady XAP envelope | PlayReady-protected XAP packages | Yes | No |

### Encodings

| Extension | Format | Description | Read | Write |
|---|---|---|---:|---:|
| `.uue`, `.uu`, `.b64` | UUencode | Classic and GNU Base64 blocks; flat member create and update | Yes | Yes |

### Binaries

| Extension | Format | Description | Read | Write |
|---|---|---|---:|---:|
| `.exe` | DOS MZ executable | DOS MZ executables | Yes | No |
| `.exe`, `.dll` | Windows NE executable | 16-bit Windows NE | Yes | No |
| `.exe`, `.dll` | OS/2 NE executable | OS/2 NE | Yes | No |
| varies | Windows PE executable | 32-bit and 64-bit PE | Yes | No |
| varies | OS/2 LE executable | OS/2 LE | Yes | No |
| varies | OS/2 LX executable | OS/2 LX | Yes | No |
| `.xbe` | Xbox XBE executable | Original Xbox XBE | Yes | No |
| `.xex` | Xbox 360 executable | XEX1 and XEX2 | Yes | No |
| `.dxbc` | DirectX Shader Container | Version 1 containers; validated chunk table and lazy raw chunk access | Yes | No |
| `.xdbf`, `.gpd` | Xbox 360 database | Version 1.0 records, title string, and image records | Yes | No |
| `.xnb` | XNA / MonoGame content | Versions 4 and 5; uncompressed, LZX, and raw LZ4; metadata and serialized root asset, with Color Texture2D preview | Yes | No |
| `.resx` | .NET resource XML | .NET XML resource files | Yes | No |
| `.msi`, `.msm`, `.msp`, `.mst`, `.pcp`, `.cub` | Windows Installer CFB | Windows Installer compound files | Yes | No |
| `.msg` | Outlook message | Outlook MSG files | Yes | No |
| varies | Apple resource fork | Generic Apple resource records, including data-fork resource maps | Yes | No |
| varies | OS/2 FEALIST | OS/2 extended-attribute files | Yes | No |
| varies | Microsoft Compress | SZDD variants A and B | Yes | No |
| varies | Microsoft KWAJ compression | Methods 0, 1, 2, 3, and 4 | Yes | No |
| varies | Microsoft XBCOMPRESS | LZMS and MSZIP | Yes | No |
| `.pup` | PlayStation 3 update package | Version 1 | Yes | No |
| `.img` | Siemens ProSave image | Siemens HMI image | Yes | No |
| `.fwf` | Siemens ProSave firmware | Siemens HMI firmware | Yes | No |
| `.fsf` | Siemens file system image | Filesystem content and metadata | Yes | No |
| varies | Siemens ADONIS / SMART_OS | ADONIS and SMART_OS images | Yes | No |
| `.hex`, `.ihx` | Intel HEX | Data records, checksums, and segment or linear addresses | Yes | Yes |
| varies | Windows CE ROM / IMGFS | Windows CE ROM and IMGFS variants | Yes | No |
| varies | Windows Registry hive | Windows Registry hive files | Yes | No |
| `.bcd` | Windows boot configuration | Boot entry data | Yes | No |

## Decoder

The Decoder interprets raw files and payloads from Reader formats. The tables
list the raw input formats that the Decoder recognizes. For embedded resource
types, see [Resource compatibility](formats/RESOURCE_COMPATIBILITY.md).

### Images

| Extension | Format | Description |
|---|---|---|
| `.png` | Portable Network Graphics | Raster image format |
| `.jpg`, `.jpeg` | Joint Photographic Experts Group image | JPEG image |
| `.pcx`, `.pcc` | ZSoft raster image | PCX and PCC variants |
| `.gif` | Graphics Interchange Format | GIF87a and GIF89a variants |
| `.bmp`, `.dib` | Windows DIB, CORE header | 12-byte `BITMAPCOREHEADER`, 1-, 2-, 4-, 8-, and 24-bit pixels |
| `.bmp`, `.dib` | Windows DIB, INFO header | `BITMAPINFOHEADER` and later headers, 1-, 2-, 4-, 8-, 16-, 24-, and 32-bit pixels, `BI_RGB`, `BI_RLE4`, `BI_RLE8`, and `BI_BITFIELDS` |
| `.bmp`, `.dib` | OS/2 bitmap 1.x | 1-, 2-, 4-, and 8-bit indexed images |
| `.bmp`, `.dib` | OS/2 bitmap 2.x | `BITMAPINFOHEADER2`, uncompressed, monochrome Huffman 1D, RLE4, RLE8, and RLE24 |
| `.arr`, `.bmp` | OS/2 bitmap array | Array containers with one or more bitmap images |
| `.tif`, `.tiff` | Tagged image file format | Raster image format |
| `.ico` | Icon image | Windows icon image |
| `.cur` | Cursor image | Windows cursor image |
| `.wmf` | Windows Metafile | Metafile image |
| `.emf` | Enhanced Metafile image | Vector image format |
| — | Doom patch lump (`WAD_DOOM_PATCH`) | Column-post picture with the palette from `PLAYPAL` |
| — | Doom sprite lump (`WAD_DOOM_SPRITE`) | Column-post sprite with the palette from `PLAYPAL` |
| — | Doom wall texture definitions (`TEXTURE1`, `TEXTURE2`) | Composite textures built from `WAD_TEXTUREX`, `PNAMES`, and Doom patch lumps |
| — | Siemens / Microwindows `MWIMAGEHDR` | Raw indexed and true-color images, 1, 2, 4, 8, 16, 24, and 32 bpp |
| — | Xbox `XBE_LOGO_RLE` | Raw run-length encoded Xbox logo image |

### Structured data

| Extension | Format | Description |
|---|---|---|
| — | Android Binary XML (AXML) | Compiled Android XML resources, including `AndroidManifest.xml` |

### Fonts

| Extension | Format | Description |
|---|---|---|
| `.fnt` | Windows bitmap font | Versions 1.0, 2.0, and 3.0 |
| `.fnt` | OS/2 bitmap and outline font | OS/2 font data |
| `.bdf`, `.afb` | Bitmap Distribution / Sun ASCII font | BDF and AFB variants |
| `.vfont` | Berkeley / Sun VFONT | Berkeley and Sun VFONT bitmap fonts |
| `.fb` | Sun X11/NeWS binary bitmap font | Legacy and extended CJK tables |
| `.dwf` | DECwindows Font (DWF), legacy | ULTRIX/UWS 75 dpi and 100 dpi compiled bitmap fonts |
| `.dwf` | DECwindows Font (DWF), MIT X11 | MIT X11 compiled bitmap font variant |
| `.dwf` | DECwindows Font (DWF), CompX10 | DEC CompX10 compiled bitmap font variant |
| `.bmf` | ByteMap font | Versions 1.1 and 1.2 |
| `.psf` | PC Screen font | Versions 1 and 2 |
| `.pcf` | Portable compiled bitmap font | PCF font data |
| `.snf` | X11 server font | Version 4, little-endian |
| `.fzx` | ZX Spectrum proportional font | Proportional bitmap font data |
| `.pk` | TeX Packed Font (PK) | Short, extended, and long packet forms |
| `.afm` | Adobe font metrics | Outline rendering is not supported |
| `.pfm` | Printer font metrics | Binary metrics; outline rendering is not supported |
| `.pfa`, `.pfb` | PostScript Type 1 | ASCII PFA and binary PFB |
| `.ps` | PostScript Type 3 | Type 3 fonts with supported path operators |
| `.svg` | SVG Font | SVG 1.1 embedded fonts |
| `.sfn` | Scalable screen font | SFN2 fonts and SFNC collections |
| `.ttf`, `.ttc` | TrueType | TrueType fonts and collections |
| `.otf`, `.ttc` | OpenType | CFF/CFF2 OpenType fonts and collections |
| `.woff` | Web Open Font Format | WOFF 1.0 |
| `.woff2` | Web Open Font Format 2 | WOFF2 |
| `.eot` | Embedded OpenType | EOT 1.0, 2.0.1, and 2.0.2 |
| `.shp`, `.shx` | AutoCAD source / compiled font | SHP source fonts and compiled SHX fonts |
| `.fch`, `.tfch` | Amiga bitmap font | FCH and supported TFCH font data |
| `.anim` | Amiga AnimFont | IFF FORM ANIM brush fonts |
| — | Apple resource-fork font records | sfnt, NFNT/FONT bitmap strikes, and FOND metadata |
