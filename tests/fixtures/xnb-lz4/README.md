# XNB LZ4 Sample

`white-1.xnb` and its manifest are from the CNA project test corpus. The
manifest records that the uncompressed payload is byte-identical to the
MonoGame test asset at `Tests/Assets/Textures/white-1.xnb`; only the container
compression was produced with upstream `liblz4` 1.10.0.

Source and provenance: <https://github.com/libcna/cna/tree/develop/tests/assets/xnb/monogame/windows/lz4>

The manifest records the expected XNB version, platform, compression flag,
root reader, payload length, and SHA-256 digest.
