# lwext4 source

This directory contains selected files from `gkostka/lwext4` at revision
`58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`.

The upstream source and header files carry BSD-3-Clause notices. Peare does not
include the upstream GPLv2 `ext4_extent.c` or `ext4_xattr.c` modules.
`src/ext4_extent.c` is a Peare-specific implementation. The build includes the
upstream `ext4_mkfs.c` module and omits `ext4_mbr.c`.

Peare enables extents and journaling. It disables xattr support. The Writer
accepts clean Ext2, Ext3, and Ext4 volumes with supported feature flags listed
in Peare's format documentation.
Peare's Ext4 case-folding support uses generated Unicode 12.1.0 tables. See
`src/ext4_casefold.c` and `UNICODE-LICENSE.txt`.
It rejects dirty volumes, orphan inodes, external journals, and unsupported
features. It creates Ext2, Ext3, and Ext4 volumes and supports CUD operations.
All six CUD operations passed on disposable copies of both real CHR 7.20.6
partitions. Ext3 CUD passed in fixed and dynamic VHD, fixed and dynamic VHDX,
and all five tested VMDK variants. Creation passed through the Writer ABI for
fixed and dynamic VHD, fixed and dynamic VHDX, monolithic and split sparse VMDK,
and raw MBR/GPT disks. Sleuth Kit 4.14 independently checked file-system types
and recovered allocated files and payloads. This validation does not establish
support for every valid Ext2, Ext3, or Ext4 feature combination.
