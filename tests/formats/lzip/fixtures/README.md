# Lzip fixture

`docs.tar.lz` was created from `tests/formats/tar/fixtures/docs.tar` with GNU
Lzip 1.26, built from its official source release. The command was:

```text
lzip -6 -c docs.tar > docs.tar.lz
```

GNU Lzip verifies the member and decodes it to bytes identical to the source
TAR. WinRAR 6.21 also extracts the sample. The source TAR contains project-owned
documentation. No GNU Lzip implementation code is included in Peare.
