# XAR Samples

The upstream [`go-filesystems/xar`](https://github.com/go-filesystems/xar) project created these archives with
`/usr/bin/xar 1.8dev`. Its generator lists and extracts each archive with that
tool, then compares every extracted file with its source.

`mixed.xar` covers zlib, stored, empty, and nested file entries. It also has a
symlink and extended attributes. `bzip2.xar` covers bzip2 members. `store.bin`
is the source file for the stored member in `mixed.xar`.

The upstream project distributes these test files under BSD 3-Clause. See
`LICENSE.txt` for the license and required copyright notice.
