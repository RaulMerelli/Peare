# NSIS Validation Samples

The validation set covers official NSIS 1.x, 2.x, and 3.x releases.

The NSIS 1.98 compiler tools come from the official NSIS 1.98 distribution.
Two small installers were built with its zlib and bzip2 compilers. They contain
`README.md` and `docs/SUPPORTED_FORMATS.md` from this repository. ABI tests
compare both files with their source files. GUI offscreen tests open both files.

The official NSIS 1.98 installer is tested through the ABI and GUI. The ABI
test checks its format, entry count, and selected paths.

The ABI suite tests official setup programs from NSIS 2.0 through 2.07. It
compares their payload hashes with independent 7-Zip extractions. These old
setups use run-time variables in some output paths. Peare keeps unresolved
variables in those paths and does not guess their run-time values.
The NSIS 2.0 alpha 1 through beta 3a setup previews are not part of this stable
release set. Peare does not open these early preview builds.

The local corpus has installers built with the official compiler for each
available NSIS 2.07 through 3.13 release. It also includes NSIS 2.07 beta 0.
Each installer contains `README.md` and `docs/SUPPORTED_FORMATS.md` from this
repository. The ABI test checks both payload hashes against the source files.
The six NSIS 3.13 samples cover zlib, bzip2, and LZMA in solid and non-solid
modes. The repository does not store these binary samples.

The full NSIS 3.13 installer is the official release setup program. 7-Zip
extracts its contents to create an independent oracle. The ABI test compares
the extracted payload sizes and SHA-256 hashes.

The setup program and generated installers remain local validation files. Do
not add them to the repository.
