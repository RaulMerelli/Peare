# SZDD Mode B Samples

The local sample corpus comes from the Windows 3.1 build 026 archive at
https://archive.org/details/windows-3.1-build-026. The archive contains six
floppy images. 7-Zip 19.00 extracted the archive and the files from each image.

The corpus contains 371 files with the SZDD signature and mode byte `B`. The
archive is copyrighted. Do not add the archive, disk images, or extracted files
to this repository. The test reads files from a local directory configured by
`PEARE_SZDD_B_SAMPLE_ROOT`.

Windows `expand.exe` rejects mode B. The ABI test therefore checks successful
decoding, the declared output size, and known DOS executable signatures. It
does not compare every decoded byte with an independent extraction.
