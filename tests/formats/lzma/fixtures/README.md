# LZMA Fixture

`README.md.lzma` was created from the repository's `README.md` with XZ Utils
5.8.3, an independent encoder:

```text
xz --format=lzma --stdout README.md > README.md.lzma
```

The fixture uses an unknown unpacked size and an end marker. XZ Utils 5.8.3
decompresses it to bytes identical to the source file. The source is project
documentation and is distributed in this repository.

`docs.tar.lzma` was created from `tests/formats/tar/fixtures/docs.tar` with
XZ Utils 5.6.4, an independent encoder:

```text
xz --format=lzma --stdout tests/formats/tar/fixtures/docs.tar > docs.tar.lzma
```

The compressed sample passes `7z t` and decompresses to bytes identical to the
source TAR file. The source TAR was produced by GNU tar from project-owned
documentation; its provenance is recorded in
`tests/formats/tar/fixtures/README.md`.
