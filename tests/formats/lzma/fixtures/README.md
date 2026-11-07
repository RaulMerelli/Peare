# LZMA Fixture

`README.md.lzma` was created from the repository's `README.md` with XZ Utils
5.8.3, an independent encoder:

```text
xz --format=lzma --stdout README.md > README.md.lzma
```

The fixture uses an unknown unpacked size and an end marker. XZ Utils 5.8.3
decompresses it to bytes identical to the source file. The source is project
documentation and is distributed in this repository.
