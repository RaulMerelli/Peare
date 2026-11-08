# Android APK sample

The test uses the official F-Droid client APK, version 1.23.2.

- Source: https://f-droid.org/packages/org.fdroid.fdroid/
- Download: https://f-droid.org/repo/org.fdroid.fdroid_1023052.apk
- SHA-256: `985f5181d48bb6bafd54083a048b391271e0ab28385881cc41294fb01a222762`
- Signature check: Android SDK `apksigner` verifies schemes v1, v2, and v3.
- Signer oracle: one signer, common name `Ciaran Gultnieks`, and certificate
  SHA-256 `43238d512c1e5eb2d6569f4a3afbf5523418b82e0a3ed1552770abb9a9c9ccab`.
- Independent manifest check: Android SDK `aapt dump xmltree` reports package
  `org.fdroid.fdroid`, version `1.23.2`, and minimum SDK 23.

The APK is not stored in this repository. Set `PEARE_ANDROID_SAMPLE` to its
local path to run the tests.

The v3.1 sample uses the F-Droid APK above as input. Android SDK `apksigner`
36.1.0 signed it with two test keys and a signing-certificate lineage. The tool
verifies v1, v2, v3, and v3.1, with one signer for each SDK range. The sample and
test keys stay outside the repository. `apksigner rotate --in` reads the
certificate lineage from the sample. Set `PEARE_ANDROID_V31_SAMPLE` to its local
path to run the v3.1 tests.
