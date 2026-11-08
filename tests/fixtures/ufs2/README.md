# UFS2 Fixture

`ufs2.raw` is `test_data/ufs2.raw` from the dfVFS project. It is distributed
under Apache License 2.0. A copy of the license is included here. The fixture
is unchanged and has SHA-256
`EE5D99E5E60AA566B9E4DD0A5341C1E2B433CBBFCEBFFBDBF0AD2A3E2153CD67`.

Source: https://raw.githubusercontent.com/log2timeline/dfvfs/main/test_data/ufs2.raw
Project and license: https://github.com/log2timeline/dfvfs

Sleuth Kit 4.15.0 identifies a BSD disklabel at sector 1 and UFS2 at sector 16.
It reports volume `ufs2_test`, root inode 2, and a 116-byte `passwords.txt`.
The ABI and GUI tests compare the root listing and file SHA-256 with Sleuth Kit.
