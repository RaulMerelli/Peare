# GZIP fixture

`README-plus-docs.tar.gz` contains two concatenated GZIP members. 7-Zip 19.00
created the first member from this repository's `README.md`. It created the
second member from `tests/formats/tar/fixtures/docs.tar`. Windows `copy /b`
concatenated the two independent GZIP streams. Python's standard GZIP reader
verified that the decoded bytes equal the two source files in order.

The decoded payload is 54,211 bytes. Its SHA-256 is
`281291583401fd85d44562729fb76f723f9b5ce562d665c06b7258ddcf8ac431`.
