# ZIPX Fixtures

The `.zipx` files in this directory were created from the repository's
`README.md` with 7-Zip, an independent archiver:

```text
7z a -tzip -mm=BZip2 README-BZip2.zipx README.md
7z a -tzip -mm=Deflate64 README-Deflate64.zipx README.md
7z a -tzip -mm=Deflate64 ZipModule-Deflate64.zipx src/opener/modules/ZipModule.cpp
7z a -tzip -mm=LZMA README-LZMA.zipx README.md
7z a -tzip -mm=PPMd README-PPMd.zipx README.md
7z a -tzip -mm=PPMd -mx=9 -mmem=1m README-PPMd-cutoff.zipx README.md
wzzip -ep README-PPMd-freeze.zipx README.md
7z a -tzip -mm=xz README-XZ.zipx README.md
python -c "from zipfile import ZipFile, ZIP_ZSTANDARD; ZipFile('README-Zstandard.zipx', 'w', compression=ZIP_ZSTANDARD, compresslevel=3).write('README.md', 'README.md')"
```

The four original samples, both Deflate64 samples, and both PPMd samples were
created with 7-Zip 19.00.
WinZip Command Line Add-On 6.0 created the PPMd freeze sample. Its method 98
entry has neither the restart nor the cutoff flag set.
The Zstandard sample was created and reopened with CPython 3.14.10. It uses ZIP
method 93. The six method-10 DCL Implode samples were created with
`zipfile-dclimplode` 0.0.3.1 and `dclimplode` 0.0.1.0. They cover binary and
ASCII literals with 1, 2, and 4 KiB dictionaries. Each sample contains only the
source README.
The README payload in the committed method samples is 2,062 bytes with SHA-256
`84f1b1405488f766cfeae58d72cd7d73aef6d38654bb16887f13ef337fd659a0`. The ABI
tests use this fixed source digest so later README edits do not invalidate the
independent archive samples.

The external TurboPower Abbrevia test-data corpus contains 29 ZIPX method-97
members and their original WAV files. Its redistribution terms are not stated,
so it is not included here. See `docs/testing/SAMPLE_PROVENANCE.md` for test
results and the environment variables used to run its ABI comparison.
