# WIM Fixtures

`test.swm` and `test2.swm` are a synthetic two-part split WIM set. The set was
created with the official wimlib 1.14.5 Windows binary from
https://wimlib.net/downloads/. PowerShell generated four 400 KiB payload files
with `RandomNumberGenerator`. `wimlib-imagex capture` stored them without
compression. `wimlib-imagex split` used a 1 MiB part size.

The payload hashes are recorded in `tests/test_wim_swm_abi.py`. The split set
was joined with wimlib and the joined WIM passed the 7-Zip 19.00 archive test.
The wimlib binary is a local test tool. It is not a Peare dependency.

`wim-small.swm` and `wim-small2.swm` were captured and split with wimlib
1.14.5, without compression and with a 0.001 MiB split size. The input had
an empty directory and two copies of the original version of this README,
one at the root and one in `nested`. The input files have SHA-256
`DB58E70BF344B11D839520E667F91292E19601A9FBFBC86ED8CE74090582E1FB`.
The paths were independently listed with `wimlib-imagex dir`.

| File | Size | SHA-256 |
| --- | ---: | --- |
| `test.swm` | 821,518 | `F91DF745F91BE14ADB4C5BB3D6C8816C30E2F27A3ECDD238574217D77C65287C` |
| `test2.swm` | 820,308 | `2DC8B4E66C822E6493D768BF72DBFC08A974975847A3AB48BB69D22138F8B37C` |
| `wim-small.swm` | 1,666 | `5951481FE28411F4D4637898CCD9E6E259007C33F827BB00BECB7DC40B173D8C` |
| `wim-small2.swm` | 1,858 | `2ECDDF5A78EE93E0896B88E12289262029AD34F8F8D9EA461FAD7709A284852C` |
