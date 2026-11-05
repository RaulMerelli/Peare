# Font ABI fixtures

The `spleen-*` files are unchanged outputs from Spleen 2.2.0 by Frederic
Cambus. The upstream project publishes BDF, PCF, PSF, Windows FON, Apple dfont,
OTF, WOFF, and WOFF2 files. The BSD 2-Clause license is included in
`Spleen-LICENSE.txt`.

`spleen-12x24.fzx` is an independent Monobit conversion of the upstream
Spleen 12x24 BDF sample. Its source and conversion tool are recorded below.

The `4x6*` samples come from the Monobit test corpus at commit
`040b1738085ce7967478499eef2835d2cffc95fd`. Its `tests/fonts/README.md` records
the public-domain source font and the independent conversion tools. The source
BDF is from Markus Kuhn's Unicode font collection. The VFONT files were made
with psftools 1.1.1. The other conversion-tool versions are not recorded by the
upstream corpus. `6x13.fon` is a public-domain font from Simon Tatham's font
collection. `cmr10.96pk` is an unchanged Computer Modern file from CTAN under
the Knuth License. `wbfont_prop.font` is freeware by Keith Halstead; its
required readme is included as `wbfont-readme.txt`. `hershey.svg` is a Monobit
conversion of the Hershey font data; its required acknowledgements and use
conditions are included as `hershey-readme.txt`.

`Spleen-ascii.sfn` and `Spleen-latin1.sfn` are SFN2 fonts converted by upstream
SSFN `sfnconv` from the included Spleen OpenType font. `Spleen-collection.sfn`
is an SFNC collection made by the same independent tool from both ranges.
Their source, conversion command, tool version, and license are recorded below.
`bende.sfn` is another upstream gzip-wrapped SFN2 sample. Its MIT license is
included as `SSFN-MIT-LICENSE.txt`.
