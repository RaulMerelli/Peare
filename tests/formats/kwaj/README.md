# KWAJ validation samples

The KWAJ ABI tests use local samples. They do not store sample files in this
repository.

The method 0, 1, and 2 samples were created by
[Cabriolet 0.3.0](https://github.com/omnizip/cabriolet). The tool compressed
the project `README.md` with each method and stored the original length.
[Pylibmspack 0.4.0](https://github.com/bwhitn/pylibmspack) decoded each sample
to an independent output file.

The method 3 corpus contains 549 compressed files from the Windows for
Workgroups 3.11 distribution. The distribution came from the Internet Archive
[item](https://archive.org/details/windowsforworkgroups). 7-Zip 19.00
extracted the floppy images and files. Pylibmspack 0.4.0 decoded each KWAJ file.

The method 4 corpus contains 311 files from the Microsoft Word 6 for Windows
floppy images in the Internet Archive
[item](https://archive.org/details/ms-word-6-win-1). 7-Zip 19.00 extracted
the images and files. Pylibmspack 0.4.0 decoded each KWAJ file. 111 files
contain multiple MS-ZIP blocks.

The 36 method 0 header cases come from the
[libmspack `kwajd` test corpus](https://github.com/kyz/libmspack/tree/master/libmspack/test/test_files/kwajd).
Pylibmspack 0.4.0 decoded each valid case. The other upstream cases test
header rejection and are not part of the valid-payload count.

The archives and decoded outputs remain outside this repository. The ABI test
uses `PEARE_KWAJ_SAMPLE_ROOT`, `PEARE_KWAJ_ORACLE_ROOT`,
`PEARE_KWAJ_UPSTREAM_SAMPLE_ROOT`, `PEARE_KWAJ_UPSTREAM_ORACLE_ROOT`,
`PEARE_KWAJ_COMPRESSED_SAMPLE_ROOT`, `PEARE_KWAJ_COMPRESSED_ORACLE_ROOT`,
`PEARE_KWAJ_MSZIP_SAMPLE`, `PEARE_KWAJ_MSZIP_ORACLE`,
`PEARE_KWAJ_WORD6_SAMPLE_ROOT`, and `PEARE_KWAJ_WORD6_ORACLE_ROOT`.
