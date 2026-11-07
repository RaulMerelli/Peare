# CramFS validation samples

The validation images stay outside this repository. Obtain them from the listed
independent sources and extract their contents with 7-Zip 19.00 before running
the ABI test in `tests/test_cramfs_samples_abi.py`.

| Variant | Sample source | Independent extraction |
|---|---|---|
| Little-endian, version 2 | [util-linux fixture](https://raw.githubusercontent.com/util-linux/util-linux/master/tests/ts/cramfs/cramfs-little.img) | 7-Zip |
| Big-endian, version 2 | [util-linux fixture](https://raw.githubusercontent.com/util-linux/util-linux/master/tests/ts/cramfs/cramfs-big.img) | 7-Zip |
| U-Boot-prefixed vendor image | [Dahua firmware directory](https://data.nag.wiki/site/data-files/Dahua/Firmware/DHI-HCVR4116HS-S2/recovery%20files/) `custom-x.cramfs.img` | 7-Zip |
| U-Boot-prefixed vendor image | [Dahua firmware directory](https://data.nag.wiki/site/data-files/Dahua/Firmware/DHI-HCVR4116HS-S2/recovery%20files/) `logo-x.cramfs.img` | 7-Zip |
| Crestron AM-100 firmware, CramFS at byte 512 | [Firmware ZIP](https://p.widencdn.net/fu0pzc/software_airmedia_am-100_1.5.0_am-101_2.6.0_firmware), file `AM-100_firmware_1.5.0.4_6506508_WM8440.img`; SHA-256 `1ef9cee502e8cb43880531adc90faa151552e1f3ce1f8ae7823fe44396321f93` | 7-Zip 19.00 |

The Dahua firmware also contains files named `romfs-x.cramfs.img` and
`web-x.cramfs.img`. They have SquashFS signatures and are not CramFS samples.
Do not use their names alone to classify filesystem contents.

The Crestron image contains Linux device nodes under `/dev`. 7-Zip extracts
these as empty files. Peare lists them as zero-byte entries with their device
type. Device major and minor numbers are not exposed. The ABI test compares all
extracted paths and file payloads.

Set the matching `PEARE_CRAMFS_*_SAMPLE` and `PEARE_CRAMFS_*_ORACLE`
environment variables. The oracle path must be the directory produced by 7-Zip.
