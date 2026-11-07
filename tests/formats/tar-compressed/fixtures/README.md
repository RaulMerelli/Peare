# Compressed TAR Fixtures

7-Zip 19.00 created all samples from `tests/formats/tar/fixtures/docs.tar`.
It used GZip for `docs.tgz` and BZip2 for `docs.tbz` and `docs.tbz2` at level 9.
Each decoded payload matches the source TAR byte for byte.
