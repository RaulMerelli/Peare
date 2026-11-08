# Android super image samples

`super.img` and `super_empty.img` are AOSP Fastboot test data from
`platform/system/core/fastboot/testdata`, branch `main`:
https://android.googlesource.com/platform/system/core/+/refs/heads/main/fastboot/testdata/

The AOSP `make_super_images.sh` script creates `super.img` with `lpmake` and
embeds `system.img` as `system_a`. The image is 65,536 bytes. LpUtil 2026-04-17
read it as LP metadata version 10.0 and extracted `system_a` as 8,192 bytes.
`system_a.img` is that unchanged LpUtil extraction and is the independent ABI
and GUI payload oracle. Its SHA-256 is
`d208f0efde13c321ca4e0fa1480d71edaf6da5529f15ca052c26d16edc965d3b`.

The AOSP test data uses Apache License 2.0. The license text is in `LICENSE`.
`super.img` SHA-256: `4a81fa41d550435c739cb2d9ff8547bb00194d84513f5f1c1122c345791f83c6`.
`super_empty.img` SHA-256: `4fbee3bbb59e157eab4b8d8e3b49f66746c906ce57929b1a225f2e5afdd0d739`.
