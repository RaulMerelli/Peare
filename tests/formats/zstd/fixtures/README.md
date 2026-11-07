# Zstandard Reader Samples

- `readme.zst` was produced from the repository `README.md` by the official
  Zstandard 1.5.7 Windows command-line encoder with frame checksums enabled.
- `readme-no-check.zst` was produced from the same input without a frame
  checksum.
- `docs.tar.zst` was produced by the same encoder from the GNU tar fixture at
  `tests/formats/tar/fixtures/docs.tar`.
- `empty.zst` was produced by the same encoder from an empty input file.
- `multi-frame.zst` concatenates the independently encoded `readme.zst` and
  `docs.tar.zst` frames. The official Zstandard 1.5.7 CLI validated the samples.

The input files are project-owned or already documented under the TAR fixture.
