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

- Microsoft ReFS, with version-specific samples and feature coverage.
- Broader UFS1/FFS and UFS2/FFS2 coverage beyond the verified Solaris and BSD samples.
- ZFS pools and datasets, including feature flags and pool-version differences.
- Classic HFS (Mac OS Standard), in addition to the supported HFS+ and HFSX variants.
- APFS containers and volumes, including snapshots and encryption where keys are available.
- F2FS, Minix, ReiserFS, BeFS, and other Unix file systems found in local images.
- JFFS2 and UBIFS images, including their flash and UBI layer requirements. Treat EROFS as a read-only target unless its format defines a safe write workflow.
- Early NTFS v1.x volumes from Windows NT 3.1 and 3.5.
- Cisco ASDM `.img` files.
- Apple floppy images: DC42 and DART.
- DMS Amiga disk images.
- VDI and Parallels HDD variants, including differencing and snapshot layouts.
- QCOW and QCOW2 writing, including backing files and external data files.
- VHD and VHDX differencing chains and snapshot layouts.
- LUKS1 and LUKS2 block-encryption containers. Define key handling before adding unlock or write support. Never infer the inner file system from encrypted data.
- Additional file-system write support beyond FAT and Ext.
- Btrfs Zstandard compression.
- XFS and SquashFS XZ or LZO compression.
- NTFS LZNT1 decompression for compressed `$DATA` attributes.
- UDF metadata, sparable, and virtual partition maps.

### Backup and imaging

- Acronis TIB and TIBX.
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
