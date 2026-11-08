# Android Sparse Fixture

The two binary files are distributed in the Android Open Source Project
`platform/external/avb` test data. The sparse image contains RAW, DONT_CARE,
and FILL chunks. The adjacent raw image is the decoded reference published by
the same project. The upstream test verifies the image with `simg2img` and
documents its contents.

Source: https://android.googlesource.com/platform/external/avb/+/refs/heads/main/test/data/
License: MIT or Apache-2.0, as stated by the upstream LICENSE file included here.

| File | Size | SHA-256 |
|---|---:|---|
| `test_file.bin.sparse` | 24668 | `4dd6ad319ded0d346440e0a63a73dba3e25f8989b0fb9ac6072c0ae6b54deed0` |
| `test_file.bin` | 61440 | `132332c4dd311b60b4d882425f1b4e7b0a6b06976fbd4665cc11f7187599195a` |
