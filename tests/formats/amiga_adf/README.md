# Amiga ADF samples

The test suite reads samples from `PEARE_AMIGA_ADF_DIR`. The samples are not
included in this repository.

| File | Source | Creation method | SHA-256 |
| --- | --- | --- | --- |
| `testofs.adf` | [ADFlib](https://github.com/adflib/ADFlib) public test data | Supplied by the ADFlib project | `b4a533173655f55785b3de2303b94893d79e970138a7755eb334d1a87746aa46` |
| `testffs.adf` | [ADFlib](https://github.com/adflib/ADFlib) public test data | Supplied by the ADFlib project | `f0213c4460d98dd6fe0eef228887bb659cf0bac86bc6180d8c4e5dbc8da868f2` |
| `win32-names.adf` | [ADFlib](https://github.com/adflib/ADFlib) public test data | Supplied by the ADFlib project | `059dbe30a2867b9f56693055681d6305e2365bcb1d3d3cd1b21792ee638d468d` |
| `bootdisk-amiga-m68k.adf` | [AROS nightly releases](https://aros.sourceforge.io/cgi-bin/files?lang=en&type=nightly2) | AROS-provided boot floppy, extracted from the `amiga-m68k` LHA release | `6001ada464bb1dfd9dd8309c6b897ab2b8c429e498e9141cd478cc808cd9f2e5` |
| `AROS-bootdisk-amiga-m68k.adz` | The AROS image above | Compressed with 7-Zip 19.00 using GZIP level 9; independently extracted and checked against the ADF | `23aa739c49801a92329acdbcf0ffa54ce0b6a84698ec3709a808bbd1361f41cb` |

The payload hashes in the ABI tests were compared with ADFlib `unadf` output.
The test images cover OFS, FFS, directory-cache FFS, nested directories, empty
files, colliding directory hash slots, and names that are unusual on Windows.
