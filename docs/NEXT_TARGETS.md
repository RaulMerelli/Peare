# Desired Targets

This list combines planned format work, improvements to existing readers, and
readers that need sample validation. It is not a schedule or a support claim.
The [format matrix](SUPPORTED_FORMATS.md) describes current support.
Add reader support first. Plan writer support after real-sample reader coverage.

## New formats and variants

### Industrial automation projects

- TIA Portal projects: versioned `.apXX` project files and their project folders.
- Browse project files and HMI assets as a read-only project tree. Do not claim that Peare can validate or run PLC logic.
- SIMATIC WinCC Classic projects: `.MCP` project files and their project folders.
- Browse documented project content such as `GraCS`, `CommonArchiving`, `.MDF`, and `RT.MDF` files. Treat database files as opaque payloads until their formats are understood.
- Include WinCC engineering data stored inside TIA Portal projects in the TIA project view. Do not treat it as a separate archive format.
- Account for version differences. Confirm additional project and database variants with versioned samples.

### File systems and disk images

- This is a broad inventory of known filesystem families and disk-image formats, not a support claim or a delivery order. The current support matrix remains authoritative. Names that identify a network service, virtual/pseudo filesystem, access method, or storage product are retained as future source types, not misrepresented as disk-image parsers.

#### Block and removable-media filesystems

- Microsoft ReFS, with version and feature coverage; early NTFS v1.x (Windows NT 3.1/3.5); broader NTFS compressed-data coverage including LZNT1.
- Broader UFS1/FFS and UFS2/FFS2 coverage; distinguish Solaris, BSD, endian, fragment, and cylinder-group variants. Also identify IBM z/OS HFS and zFS separately from Apple HFS/HFS+.
- ZFS pools and datasets, including feature flags, pool versions, vdev layouts, and encryption; APFS containers/volumes, snapshots, and encrypted volumes when keys are available; Classic HFS/MFS.
- Bcachefs, HAMMER/HAMMER2, Btrfs (including Zstandard), NILFS, XFS (including XZ/LZO where applicable), ReiserFS/Reiser4, Minix, BeFS/OpenBFS, SkyFS, Fossil, and Plan 9 filesystems.
- Historical and platform-specific disk filesystems: ADFS/DFS, Acorn ADFS, AdvFS, AthFS/AFS, System V BFS/BootFS, z/VM BFS, CP/M, Apple DOS 3.x, ProDOS, IRIX EFS, Files-11/ODS, Flex, HPFS, HTFS, IBM JFS variants, LFS, Macintosh MFS, TiVo MFS, NetWare FS/NSS, NeXT FS, Next3, OFS/FFS/PFS/SFS and variants for Amiga, OS-9, QNX4, Reliance/Reliance Nitro, RTEMS RFS, UDF variants, VxFS, and IBM zFS.
- Existing readers also need broader variant review where applicable: FAT/FATX/VFAT and embedded variants; Ext feature combinations; XFS; SquashFS compression variants; UDF metadata, sparable, and virtual partition maps; HFS+/HFSX; UFS1/UFS2; and Android/Cisco `.img` layouts.

#### Flash, embedded, and non-volatile-memory filesystems

- 3FS; F2FS; JFFS/JFFS2; UBIFS with its UBI layer; EROFS; YAFFS; LogFS; SPIFFS; LittleFS; CHFS; JesFS; NOVA; WAFL; and Microsoft flash FFS1/FFS2 (distinct from the unrelated Amiga and BSD FFS names).
- Embedded and commercial families: Reliance, Reliance Nitro, Reliance Velocity/Edge/Assure, Segger emFile, TrueFFS, ExtremeFFS, LSFS, TFAT, Palm NVFS, and XCFiles/exFAT implementations.
- Cisco ASDM `.img`; Apple floppy DC42 and DART; Amiga DMS images. For raw-flash formats, preserve the distinction between a filesystem image, NAND/NOR geometry, and any UBI/translation layer.

#### Record-oriented, mainframe, and structured storage

- CMS filesystem/SFS for VM; Files-11 record attributes; Michigan Terminal System (MTS); OS4000 and Linked-OS; Pick OS; FlexOS/IBM 4680/Toshiba 4690 FAT record extensions.
- IBM z/OS and z/VSE datasets and access methods: VSAM, BSAM, BPAM, and QSAM; DASD volume labels and VTOC metadata. These are dataset/volume structures, not ordinary directory filesystems.
- Apple Newton Soup and database-like or application-managed stores when a real disk or sample image is available.

#### Clustered, shared-disk, distributed, and network filesystems

- Shared/clustered: CFS, CXFS, Dell Fluid/ExaFS, Blue Whale/BWFS, SAN File System, EMC Celerra HighRoad, GFS2, IBM GPFS/Spectrum Scale, Nasan, Oracle ACFS, OCFS2, QFS, ScoutFS, StorNext/CVFS, Veritas Storage Foundation, Xsan, VMFS, and clustered Files-11.
- Distributed/network and fault-tolerant: 9P, AFS/OpenAFS/Arla, DCE/DFS, DECnet FAL/DAP, NCP/NetWare, NFS/pNFS, SMB/CIFS, OS4000 Linked-OS, SFS, Coda, Windows DFS, InterMezzo, LizardFS, MooseFS, Scality, Tahoe-LAFS, OpenHarmony HMDFS, Magma, MapRFS, AvereOS, Cloudian, Amazon S3, Office Groove/DoHyki, and ChironFS.
- Parallel/object/cloud storage: BeeGFS, Lustre, PVFS/PVFS2/OrangeFS, Alluxio, CephFS/RADOS, dCache, Gfarm, GlusterFS, Google File System, HDFS, IBRIX Fusion, JuiceFS, ObjectiveFS, OneFS, OIO-FS, PanFS, Quobyte, RozoFS, SeaweedFS, XtreemFS, CloudStore/QFS, IBM Cloud Object Storage, Scality, IPFS, CRFS, POHMELFS, and DST.
- These targets need a defined acquisition model (local metadata/image, protocol client, or exported snapshot) before they can be treated as openable files. A network protocol alone is not a disk format.

#### Stacked, special-purpose, pseudo, and encrypted filesystems

- Stacked/union and boot/read-only: aufs, UnionFS, OverlayFS, AXFS, UnixWare BootFS, Linux CDFS/CDfs, experimental Compact Disc FS, CramFS, romfs, SquashFS, UMSDOS/UVFAT, and Venti/Fossil.
- Interfaces and mounted sources: davfs2/WebDAV, FTPFS, Freenet, GmailFS, GridFS, LTFS, MVFS/ClearCase, Nexfs, Barracuda, lnfs, wikifs, Venti, and other filesystem-like object or tape stores.
- Pseudo filesystems and kernel views: devfs, procfs, tmpfs, sysfs, debugfs, configfs, sysctlfs, kernfs, and WinFS. Filesystem interfaces include FUSE, LUFS, PUFFS, SSHFS, and VFS; treat these as access layers, not on-disk formats. Pseudo filesystems are live/system sources, not standalone disk images, unless an export format exists.
- Encryption/container layer: LUKS1/LUKS2, eCryptfs, EncFS, Windows EFS, AIX EFS, ZFS encryption, and historical Rubberhose. Define key handling before unlock support; never infer an inner filesystem from encrypted bytes.

#### Disk-image and virtual-disk formats

- Raw and ambiguous extensions: `.raw`, `.img`, `.bin`, `.dd`, `.dsk`, and extensionless raw images; identify by content/partition structure and samples because these suffixes are not unique formats.
- QEMU and related: raw, QCOW/QCOW2 (including backing chains, external data files, snapshots, encryption, and compression), legacy QED, legacy COW, Linux compressed loop (`.cloop`), and Bochs disk images (flat, growing, redolog, and undoable variants).
- Microsoft/Hyper-V: VHD fixed/dynamic/differencing; VHDX fixed/dynamic/differencing and parent chains; AVHD/AVHDX differencing disks; VHD Set `.vhds` (documented suffix), with `.vhdset` retained as a requested alias to check against samples and implementations.
- VMware: VMDK extent/layout variants, including monolithic and split flat/sparse, streamOptimized, hosted sparse, and VMFS-oriented variants; descriptor/extent combinations and parent chains.
- VirtualBox and other hypervisors: VDI fixed/dynamic/differencing; Parallels `.hdd` bundles and `.hds` extents; Virtuozzo/OpenVZ ploop images, deltas, and snapshot metadata; Xen XVA/VHD variants; Virtual PC/Connectix VHD.
- Apple: UDIF `.dmg`, sparse `.sparseimage`, banded `.sparsebundle`, and modern sparse ASIF images; validate container and inner partition/filesystem layers separately.
- Emulator and legacy hardfiles: Amiga `.hdf`; `.hda`; 86Box `.hdx`; PC-98 `.nhd`; generic `.dsk`; Bochs images; Apple DC42/DART; and other documented emulator-specific layouts.
- Optical/removable images and containers: ISO 9660/UDF, CUE/BIN, CCD/IMG/SUB, MDS/MDF, NRG, CHD, CSO, and other track-based or compressed disc images. Keep these distinct from hard-disk virtual formats even when they use `.img` or `.bin`.
- Include the requested suffixes explicitly in discovery/tests: `.raw`, `.img`, `.bin`, `.hdd`, `.hds`, `.qed`, `.cow`, `.vhds`/`.vhdset`, `.avhd`, `.avhdx`, `.dmg`, `.sparseimage`, `.sparsebundle`, `.hdf`, `.hda`, `.hdx`, `.nhd`, `.dsk`, plus Bochs and ploop images (whose filenames/layouts are not standardized to one suffix).
- VDI, Parallels HDD, QCOW/QCOW2, VHD/VHDX, VMDK, XVA, raw floppy/disk, and selected optical images already appear in the support matrix. For them, target the missing variants and coverage, not a duplicate format claim.

- Additional file-system write support beyond FAT and Ext.
- Btrfs Zstandard compression; XFS and SquashFS XZ/LZO coverage; UDF partition-map variants.

### Backup and imaging

- Acronis TIB and TIBX: [reader research](formats/ACRONIS_READER.md).
- Veeam VBK, VIM, and VRB.

### Firmware and executable images

- Qualcomm SBL1 images with the 80-byte SBL header and image ID `0x15`.
- ELF firmware images: ELF32 little-endian `EM_ARM`, ELF64 little-endian `EM_AARCH64`, and the observed ELF64 little-endian `e_machine=40` variant.
- UEFI firmware volumes, including the volume embedded in the Lumia UEFI ELF image.

### Unresolved local disk payloads

- Cisco CSR1000v MBR primary partition 3 (`0x83`, 34 MiB): the sample contains sparse `BSI`, license, and startup trace strings, but no identified file system or container. The record format remains unresolved.

### Packages and archives

- The Sims 3 packages (`.Sims3Pack`).
- Symbian SIS and SISX application packages.
- Android APK signature verification. Peare reports v2/v3/v3.1 signer metadata
  and the v3 certificate lineage but does not verify signatures.
- Android Binary XML manifest decoding is supported.
- Add readers for ACE, ALZ, BlackHole (BH), Brotli streams (BR), DAA 1.0,
  EGG, ISZ, PMA, PEA, XXE, and ZPAQ archives.
- Add standalone LZ4 stream support. Keep this distinct from the raw LZ4
  compression supported inside XNB files.
- Add support for generic self-extracting archive executables (SFX), including
  7-Zip SFX. NSIS installers are already supported; do not treat that as
  support for every EXE SFX layout.
- Audit encryption by container and method. 7z AES-256 and encrypted RAR4/RAR5
  headers and members are supported when a password is supplied. Document and
  implement any remaining encrypted archive variants through the public ABI.
- Validate format coverage against independent samples for ambiguous `.img` and
  `.bin` inputs. Existing support covers specific disk/floppy images and
  CUE/BIN optical images, not every file with those suffixes.
- Expand XZ support to blocks using Delta and BCJ filters; current XZ support
  accepts one LZMA2 filter per block.
- Validate ISO/UDF combinations and broaden UDF partition-map coverage. Current
  support includes Joliet, standard ISO, raw Mode 2, and UDF Type 1 partitions.
- The raised ZIP, 7z/LZMA2, ZIPX/XZ, TAR family, TGZ, GZ, XZ, LHA/LZH (including
  LH7), ISO/Joliet, ARJ, BZ2, CAB, MSI compound files, DEB, LZ/LZMA, RAR/RAR5,
  TBZ/TBZ2, TLZ, TXZ, UDF, WIM, XPI, Z, UU/UUE, ASAR, Zstandard, and NSIS
  variants already have reader support listed in the [format matrix](SUPPORTED_FORMATS.md).
  Continue testing their documented variants; do not add duplicate format
  targets. `AES` is an encryption method, not a standalone archive format.
- Reduce memory use and decode time for very large archive members.
- 7z archive files and decoded members larger than the Qt 5 `QByteArray` limit.
- Identify the `.tpz` extension shown by 7-Zip from a real sample before planning support.

### Executables, games, and graphics

- Console WAD variants with compressed lumps, including Atari Jaguar Doom and Doom 64.
- UPX-compressed executables.
- .NET resources embedded in PE files.
- In-place editing of executable resources.

## Improve existing support

- OS/2 PACK2 `fT21`; `fT19` is supported, but the reader does not accept `fT21`.
- .NET RESX typed objects and resource references.
- XBE resource structures. The current resource layout is too synthetic.
- MSI stream and table character decoding.

## Implemented, sample validation needed

The readers below are implemented. No sample verification exists yet:

- Microsoft Tape Format (MTF).
- NTBackup BKF.
- Parallels Desktop Hard Disk (HDD).

Use real samples or files created by independent tools. Do not create format
samples in test code. Do not add restricted samples to the repository. See the
[fixture policy](../tests/POLICY.md).

## References

- [Microsoft ReFS overview](https://learn.microsoft.com/en-us/windows-server/storage/refs/refs-overview)
- [Oracle Solaris UFS guide](https://docs.oracle.com/cd/E19120-01/open.solaris/819-2723/fscreate-1/index.html)
- [FreeBSD UFS and UFS2 guide](https://docs.freebsd.org/en/books/handbook/filesystems/)
- [Linux kernel file-system index](https://docs.kernel.org/filesystems/)
- [Linux kernel ext4 structures](https://docs.kernel.org/filesystems/ext4/)
- [Linux kernel F2FS documentation](https://docs.kernel.org/filesystems/f2fs.html)
- [Linux kernel UBIFS documentation](https://docs.kernel.org/filesystems/ubifs.html)
- [List of file systems](https://en.wikipedia.org/wiki/List_of_file_systems) (broad seed inventory; includes disk, flash, record-oriented, cluster, network, special-purpose, pseudo, and encrypted families)
- [QEMU disk image formats](https://www.qemu.org/docs/master/system/images.html)
- [QEMU image utility and supported formats](https://www.qemu.org/docs/master/tools/qemu-img.html)
- [Microsoft VHD Set protocol specification](https://learn.microsoft.com/en-ie/openspecs/windows_protocols/ms-rsvd/f75103f4-eaf1-446e-bef9-3e293e0e767c)
- [Apple Disk Utility disk image formats](https://support.apple.com/guide/disk-utility/create-a-disk-image-dskutl11888/22.7/mac/27)
- [System V ELF Generic ABI](https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html)
- [Qualcomm MBN and SBL header implementation in coreboot](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/chromeos-2016.05/util/qualcomm/mbn_tools.py)
- [UEFI Platform Initialization specifications](https://uefi.org/specifications)
- [EROFS project documentation](https://erofs.docs.kernel.org/en/latest/)
- [OpenZFS feature flags](https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Pool%20Structure/Feature%20Flags.html)
- [Cryptsetup LUKS1 on-disk specification](https://gitlab.com/cryptsetup/cryptsetup/-/wikis/LUKS-standard/on-disk-format.pdf)
- [Cryptsetup LUKS2 on-disk specification](https://gitlab.com/cryptsetup/LUKS2-docs)
- [TIA Portal: Opening projects](https://docs.tia.siemens.cloud/r/en-us/v21/editing-projects/creating-and-managing-projects/opening-projects)
- [TIA Portal: Working with project archives](https://docs.tia.siemens.cloud/r/en-us/v21/editing-projects/archiving-and-retrieving-projects/working-with-project-archives)
- [TIA Portal: Creating a project archive](https://docs.tia.siemens.cloud/r/en-us/v20/editing-projects/archiving-and-retrieving-projects/creating-a-project-archive)
- [WinCC V8.1: Working with WinCC, file structure of a project](https://support.industry.siemens.com/cs/attachments/109986766/WinCC_Working_with_WinCC_Part1_en-US_en-US.pdf)
- [NUP format specification](https://github.com/a55uka/nup/blob/meow/SPEC.md)
