# WIM Fixtures

`test.swm` and `test2.swm` are a synthetic two-part split WIM set. The set was
created with the official wimlib 1.14.5 Windows binary from
https://wimlib.net/downloads/. PowerShell generated four 400 KiB payload files
with `RandomNumberGenerator`. `wimlib-imagex capture` stored them without
compression. `wimlib-imagex split` used a 1 MiB part size.

The payload hashes are recorded in `tests/test_wim_swm_abi.py`. The split set
was joined with wimlib and the joined WIM passed the 7-Zip 19.00 archive test.
The wimlib binary is a local test tool. It is not a Peare dependency.

| File | Size | SHA-256 |
| --- | ---: | --- |
| `test.swm` | 821,518 | `F91DF745F91BE14ADB4C5BB3D6C8816C30E2F27A3ECDD238574217D77C65287C` |
| `test2.swm` | 820,308 | `2DC8B4E66C822E6493D768BF72DBFC08A974975847A3AB48BB69D22138F8B37C` |
