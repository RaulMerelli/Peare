# Testing

Run commands from the repository root. Test output belongs under `Peare\tmp`;
do not use the operating system temporary directory for project test data.

Peare tests call the built shared libraries through their public C ABIs. The
tests do not use private C++ interfaces.

## Sample Rules

Read [`tests/POLICY.md`](../../tests/POLICY.md) before adding a sample.
Record each sample source and creation method in the fixture README or test
metadata. Keep sample files outside the repository unless redistribution is
permitted.

Use an unchanged real file or a file created by an independent program. Do not
write test code that builds a format sample from bytes or format rules. Do not
mutate, truncate, or corrupt a sample to create another input. A test may create
payload files for a Writer operation. A Writer output may be tested as output,
but an independent program must verify it. A Peare Writer output is not Opener
sample evidence.

## Test Modules

| Test module | Coverage |
|---|---|
| `tests/test_formats.py` | Opens each eligible repository fixture through the Opener ABI and compares its resource tree with a reviewed JSON manifest. Optional QEMU tests cover empty and non-empty stream-optimized VMDKs; the non-empty test compares the complete virtual-disk bytes with the raw source. |
| `tests/test_font_samples_abi.py` | Sends standalone font bytes and raw container-resource payloads to Decoder, checks collection faces, scans configured font directories, and tests GZIP/ZIP-wrapped fonts. Opener is tested only for generic container extraction and for not classifying standalone fonts. |
| `tests/test_resource_export_abi.py` | Verifies multi-image `RT_GROUP_ICON` export as ICO through Decoder's C ABI using a real PE fixture. |
| `tests/test_pfm_samples_abi.py` | Checks real PFM metric files through Decoder text and metadata ABI calls. |
| `tests/test_zip_implode_abi.py` | Compares all members of a real Implode ZIP with 7-Zip through the Opener ABI. |
| `tests/test_zipx_methods_abi.py` | Checks Deflate64, BZip2, LZMA, XZ, Zstandard, PPMd restart/freeze/cut-off, MP3, JPEG, WavPack, and all six ZIP DCL Implode modes through the Opener ABI against source files. Optional external samples use the `PEARE_ZIPX_*` variables below. |
| `tests/test_ihex_samples_abi.py` | Compares Intel HEX data records at low and extended addresses with their source payload through the Opener ABI. |
| `tests/test_gzip_multi_member_abi.py` | Checks concatenated GZIP members through the Opener ABI against both source payloads. |
| `tests/test_tar_compressed_variants_abi.py` | Compares `.tgz`, `.tbz`, and `.tbz2` payloads with their TAR source, opens each nested TAR, and checks a `.taz` alias of an upstream `.tar.Z` sample. |
| `tests/test_wad_decode_abi.py` | Opens and decodes sprites and textures from the external Doom IWAD archive when available. |
| `tests/test_amiga_adf_abi.py` | Checks OFS and FFS directory trees, file payloads, unusual names, and a GZIP-compressed ADZ sample through the Opener ABI and GUI. Set `PEARE_AMIGA_ADF_DIR` to the local sample directory. |
| `tests/test_android_apk_abi.py` | Checks APK entries, v2/v3/v3.1 signer metadata, and Android binary XML previews through the ABI and GUI. It does not verify APK signatures. Set `PEARE_ANDROID_SAMPLE` and `PEARE_ANDROID_V31_SAMPLE` to the local APK samples. |
| `tests/test_wim_swm_abi.py` | Opens both parts of a split WIM set and checks every payload hash through the Opener ABI. `tests/formats/wim/gui-preview-cases.json` checks the same parts in the GUI. |
| `tests/test_uue_samples_abi.py` | Compares upstream libarchive classic UUencode samples with their `.Z` payloads and checks the RFC 6873 Base64 envelope through the Opener ABI. |
| `tests/test_chm_samples_abi.py` | Compares CHM members with independent 7-Zip extractions through the Opener ABI. Set `PEARE_CHM_ITSF2_SAMPLE` and `PEARE_CHM_ITSF2_ORACLE` to enable the optional ITSF v2 sample case. |
| `tests/test_lzip_samples_abi.py` | Opens GNU Lzip test samples through the Opener ABI and compares the decoded payload with the upstream source file. |
| `tests/test_szdd_variant_b_abi.py` | Opens the local Windows 3.1 build 026 SZDD mode B corpus through the Opener ABI and checks declared sizes and selected DOS signatures. Set `PEARE_SZDD_B_SAMPLE_ROOT` to the extracted sample directory. |
| `tests/test_kwaj_samples_abi.py` | Checks KWAJ methods 0–4 through the Opener ABI against libmspack output. It covers 549 authentic method 3 files, 311 Word 6 method 4 files, 111 multi-block MS-ZIP files, and independent method 0–2 samples. |
| `tests/test_xbcompress_samples_abi.py` | Compares Microsoft XBCOMPRESS LZMS, multi-block LZMS, and multi-block MSZIP samples with their original inputs through the Opener ABI. |
| `tests/test_lzma_samples_abi.py` | Opens an LZMA-alone stream from an independent XZ Utils encoder through the Opener ABI and compares its payload with the source file. |
| `tests/test_zstd_samples_abi.py` | Opens independently encoded Zstandard frames through the Opener ABI; checks checksums, empty and concatenated frames, and nested TAR dispatch. |
| `tests/test_lha_samples_abi.py` | Opens configured LHA samples through the Opener ABI and compares names and payloads with 7-Zip. |
| `tests/test_7z_methods_abi.py` | Compares 7z codecs, filters, headers, six compression levels, coder properties, and solid-block settings through the Opener ABI with 7-Zip 19.00. Set `PEARE_7Z_REAL_SAMPLE_ROOT` to compare an external solid archive corpus with 7-Zip. |
| `tests/test_rar4_samples_abi.py` | Compares RAR4 members with 7-Zip output. Covers methods, solid archives, filters, encryption, split volumes, and a compressed Unicode filename. |
| `tests/test_rar5_samples_abi.py` | Compares RAR5 methods, solid streams, filters, encrypted data and headers, and split volumes with 7-Zip through the Opener ABI. It also reads an archive with recovery data. |
| `tests/test_cab_samples_abi.py` | Compares uncompressed, MSZIP, LZX15, LZX21, and Quantum CAB members with 7-Zip through the Opener ABI. Set `PEARE_CAB_SAMPLE_ROOT` and `PEARE_CAB_QUANTUM_SAMPLE_ROOT` to run the external sample cases. |
| `tests/test_rar7_sample_abi.py` | Checks RAR7 algorithm version 1 through the Opener ABI against a real 6 GiB dictionary sample and its output CRC. |
| `tests/test_arj_samples_abi.py` | Compares ARJ methods 0-4, legacy encryption, GOST40, GOST256, GOST256L, and split-volume payloads with independent ARJ extractions through the Opener ABI. |
| `tests/test_xar_samples_abi.py` | Checks XAR corpus samples with stored, zlib, and bzip2 members through the Opener ABI. |
| `tests/test_deb_samples_abi.py` | Opens official Debian binary packages through the Opener ABI and checks nested control and data TAR members. Set `PEARE_DEB_SAMPLE_ROOT` to the local sample directory. |
| `tests/test_rpm_samples_abi.py` | Opens RPM packages from ArcaOS, Red Hat EL3, and Fedora 42 through the Opener ABI and checks CPIO members. Set `PEARE_RPM_SAMPLE_ROOT` to the local sample directory. |
| `tests/test_writer.py` | Runs CUD operations on disposable disk-image copies. Checks GZIP, BZIP2, Unix compress, LZMA-alone, Lzip, XZ, and Zstandard with independent decoders. Checks all ten Lzip levels against Python's raw LZMA decoder, member CRC and size fields, the Opener ABI, the offscreen GUI preview, and WinRAR when installed. Checks TAR.Z creation for all 16 LZW profiles and CUD on an upstream libarchive sample against 7-Zip and the Opener; checks TAR.XZ creation with all four check types and CUD on an independent XZ sample through the ABI and GUI Tree and Details views; checks TAR.LZMA at all nine levels and CUD on an XZ Utils sample through the ABI and GUI Tree and Details views. Checks TAR.ZST creation and updates with `python-zstandard`. Checks supported VMDK output with VMware when configured. Set `PEARE_ZSTD_TEST_EXECUTABLE` to a Zstandard CLI path when 7-Zip is older than 24.01. |
| `tests/test_android_package.py` | Checks the ELF architecture of native libraries in a built Android APK. It does not run the APK on a device. |
| `tests/run_tests.py` | Runs pytest and stores pytest files and Python bytecode in `tmp/test-output`. |
| `tests/run_gui_preview_tests.py` | Runs offscreen GUI previews for arbitrary source files and full resource-tree paths. |
| `tests/run_gui_writer_tests.py` | Runs a manifest-driven CUD scenario through the GUI on a disposable image copy. |

The writer tests require `pytest` and `zstandard` from PyPI. The Zstandard
package acts as an independent decoder for Zstandard Writer output.

The repository golden tests use fixtures under `tests/formats`. Each fixture
must include its source and redistribution terms. Samples with unknown origins
are not independent evidence.

## Run Tests

Build Peare with the repository build script. Then run:

```powershell
py tests/run_tests.py -q
```

The runner stores pytest files, temporary files, and Python bytecode in
`tmp/test-output`. This path is ignored by Git.

The GUI Writer runner writes its copied image, report, and screenshot to
`tmp/test-output/gui-writer` by default. The GUI preview runner uses
`tmp/test-output/gui-previews`.

## Process Crash Recovery

These optional tests use disposable copies of real disk images. Each test
starts a separate Writer process and writes a payload. The NTFS VMDK, VHDX,
and FAT16 VHD tests terminate the process after the rollback journal is durable
and the image has changed. Recovery runs through the Writer ABI and restores
the full image hash. The FAT16 dynamic VHD test also checks the original file
size. The split VMDK test uses a nine-extent disk derived from the Lumia VHDX.
It tests FAT and NTFS. For each file system, it terminates the process during
extent staging and after one extent is published but before descriptor replacement.
It checks both file-system trees, then runs another update to remove each orphan.
QEMU checks the updated descriptor and all nine extents.
The VMDK version 2 test converts the real CHR disk with QEMU's `zeroed_grain`
option. It terminates Writer during monolithic sparse updates to both Ext2 and
Ext3, then requires recovery to restore the complete image hash.
The exFAT crash test uses the 64 MiB image created by `fsutil` and `exfatprogs`.
It converts the image to monolithic sparse VMDK version 2 and writes 32 MiB so
the test can observe and interrupt the commit. Recovery must restore the full
image hash.

Set the local samples and enable the tests:

```powershell
$env:PEARE_RUN_PROCESS_CRASH_TEST = '1'
$env:PEARE_NTFS_CRASH_TEST_IMAGE = '<path-to-local-ntfs-vmdk-sample>'
$env:PEARE_VHDX_CRASH_TEST_IMAGE = '<path-to-local-vhdx-sample>'
$env:PEARE_FAT_CRASH_TEST_IMAGE = '<path-to-local-fat-vhd-sample>'
$env:PEARE_EXT_DISK_TEST_IMAGE = '<path-to-local-ext-disk-sample>'
$testOutput = Join-Path (Get-Location) 'tmp\test-output'
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
$env:PEARE_EXFAT_TEST_IMAGE = Join-Path $testOutput 'exfat-64MiB-clean.img'
$env:PEARE_QEMU_IMG = (Get-Command qemu-img -ErrorAction Stop).Source
$splitDir = Join-Path $testOutput ("split-crash-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $splitDir | Out-Null
$env:PEARE_SPLIT_VMDK_CRASH_TEST_IMAGE = Join-Path $splitDir 'lumia-split.vmdk'
& $env:PEARE_QEMU_IMG convert -p -f vhdx -O vmdk -o subformat=twoGbMaxExtentSparse $env:PEARE_VHDX_CRASH_TEST_IMAGE $env:PEARE_SPLIT_VMDK_CRASH_TEST_IMAGE
py tests/run_tests.py -q tests/test_writer.py::test_ntfs_vmdk_process_termination_recovers_disposable_copy
py tests/run_tests.py -q tests/test_writer.py::test_ntfs_vhdx_process_termination_recovers_disposable_copy
py tests/run_tests.py -q tests/test_writer.py::test_fat_vhd_process_termination_recovers_disposable_copy
py tests/run_tests.py -q tests/test_writer.py::test_split_vmdk_process_termination_cleans_unpublished_files
py tests/run_tests.py -q tests/test_writer.py::test_vmdk_v2_process_termination_recovers_disposable_copy
py tests/run_tests.py -q tests/test_writer.py::test_exfat_vmdk_v2_process_termination_recovers_disposable_copy
```

## Offscreen GUI Previews

Use one format-independent path for every GUI preview. A case gives a source
file or folder and the exact path shown in Peare's navigation tree. The path can
cross any number of nested containers, but it must select a leaf. The runner
saves one screenshot per case and fails if Peare cannot navigate to the path,
render preview content, or save a valid PNG.

On Windows, the build script stores the offscreen plugin in
`build-windows/test-plugins/platforms`. The GUI runners use this path by
default. Set `QT_QPA_PLATFORM_PLUGIN_PATH` to override it.

For a large sample set, pass a JSON manifest. Each case needs `name`, `source`,
and `tree_path`. Set `min_images` when the case must render a known number of
preview images. Use `sample_id` to group several resources from one source file.
Set `--jobs` to run cases in parallel and `--report` to save all results.

Manifest cases must include `expected_size` and `expected_sha256`. The runner
compares them with the selected payload. It also checks that the selected item
is a leaf and that loaded GUI paths have no duplicates. A zero-byte leaf can
have no preview text. Ad hoc `--case` runs can omit the ABI oracle. Their report
sets `oracle_verified` to `false`.

```powershell
py tests/run_gui_preview_tests.py `
  --exe build-windows/Release/Peare.exe `
  --case siemens-fwf "C:/samples/mobile.zip" "Mobile2ndGen/17.0/Images/KTP_4_Mobile/KTP_4_Mobile_V17_0.fwf" `
  --case standalone-font "C:/samples/font.ttf" "" `
  --case dll-font "C:/samples/fonts.dll" ".rsrc/RT_FONT/#1001/#1033"
```

The runner does not infer a format or choose a resource. Tests must provide the
same full tree path that a user can select in the GUI. A manifest can include a
`password` value for encrypted samples. Standalone font files go directly to
Decoder. Opener is used only when the source is a container; it returns the
selected record bytes without interpreting them. A complete font grid requires
one displayed cell per glyph. A blank glyph is valid and does not need visible
pixels.

The complete Opener fixture manifest is `tests/gui/opener-preview-cases.json`.
It covers every input fixture in `tests/formats/*/fixtures` and every hashable
GUI leaf. It also checks the empty-output and RAR4 Unicode cases. Intel HEX
address markers are not GUI files, so the manifest omits them. Run it after a
Windows build:

```powershell
py tests/run_gui_preview_tests.py `
  --exe build-windows/Release/Peare.exe `
  --manifest tests/gui/opener-preview-cases.json `
  --output-dir tmp/test-output/gui-previews/opener `
  --report tmp/test-output/gui-previews/opener-report.json `
  --jobs 2
```

## Archive Directory Tests

`test_archive_directory_entries_abi.py` creates small archives with 7-Zip and
WinRAR. It checks exact directory paths and file hashes through the Opener ABI.
It also checks that explicit archive directories are not lazy containers.
The test covers 7z, RAR4, and RAR5. Run it with:

```powershell
py tests/run_tests.py -q tests/test_archive_directory_entries_abi.py
```

## Offscreen GUI Writer Tests

The GUI Writer runner accepts a JSON scenario with generic create-directory,
create-file, add-files, rename, replace, and delete operations. It invokes the
GUI actions, supplies answers to their dialogs, and checks each expected path.
The test executable must be built with `tools/build/build_windows.bat`. The runner copies
the input image before it starts and verifies that the original sample hash
does not change. For descriptor-based VMDK files, it also copies every listed
extent. The GUI scenario runs with `QT_QPA_PLATFORM=offscreen`.

The FAT16 GUI test runs every CUD operation in both Tree and Details views. It
uses the local Lumia EFIESP sample from `PEARE_FAT16_TEST_IMAGE`, checks that
the current Details location stays open after each update, then reopens the
copy through the Opener C ABI and checks original and changed payload hashes.
The DOS mTCP test uses `PEARE_VMDK_TEST_IMAGE` to run the same scenario on a
real monolithicSparse VMDK and its FAT16 partition.

The CHR 7.20.6 test runs the same scenario on its real Ext2 and Ext3
partitions. The Lumia VHDX test selects its first NTFS target through the
Writer ABI and checks the matching partition in the GUI. The Ext4 depth-3 and
exFAT tests use their real local samples. Each test runs in Tree and Details
views and checks the reopened file hashes.

The Ext4 extent ABI tests use external e2fsprogs 1.47.4 and Sleuth Kit 4.14.0.
They cover initialized, sparse, and unwritten extents at depths 0-5 on
disposable copies, plus depth promotion through fragmentation on the CHR 7.20.7
Ext4 partition. Set `PEARE_EXT_DISK_TEST_IMAGE` to a local sample path, then run:

```powershell
py tests/run_tests.py -q tests/test_writer.py -k 'ext4_extent_tree_depths_replace_on_disposable_copy or chr_7207_ext4_splits_and_collapses_multilevel_extent_tree'
```

```powershell
$testOutput = Join-Path (Get-Location) 'tmp\test-output'
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
$env:PEARE_FAT16_TEST_IMAGE = '<path-to-disposable-fat16-sample-copy>'
$env:PEARE_FAT_DOS_TEST_IMAGE = '<path-to-local-dos-floppy-sample>'
$env:PEARE_VMDK_TEST_IMAGE = '<path-to-local-vmdk-sample>'
$env:PEARE_EXT_DISK_TEST_IMAGE = '<path-to-local-ext-disk-sample>'
$env:PEARE_VHDX_TEST_IMAGE = '<path-to-local-vhdx-sample>'
$env:PEARE_EXT4_DEPTH3_TEST_IMAGE = '<path-to-disposable-ext4-depth3-sample-copy>'
$env:PEARE_EXFAT_TEST_IMAGE = Join-Path $testOutput 'exfat-64MiB-clean.img'
$env:PEARE_CHR_7_20_7_LARGE_TEST_IMAGE = '<path-to-disposable-chr-large-sample-copy>'
$env:PEARE_GUI_EXE = 'build-windows/Release/Peare.exe'
py tests/run_tests.py -q tests/test_writer.py -k gui_cud_offscreen
```

The test configuration searches the standard `build-windows` output folders.
Set these variables to select explicit library paths:

```powershell
$env:PEARE_OPENER_LIB = 'build-windows/Release/PeareOpener.dll'
$env:PEARE_DECODER_LIB = 'build-windows/Release/PeareDecoder.dll'
$env:PEARE_WRITER_LIB = 'build-windows/Release/PeareWriter.dll'
```

Tests that require a library skip when that library is unavailable. Local sample
tests skip unless their documented environment variables point to existing
files. They write only to temporary copies; they do not change the original
sample.

`test_formats.py` accepts `PEARE_UPDATE_EXPECTED=1` to rewrite golden manifests.
Use this only for an independently sourced fixture. Review the tree and verify
payload hashes with an independent tool before accepting the new manifest.

## Current Font Results

The font ABI suite was run against the current built libraries on 2026-10-31.
It reported 274 passed and 13 skipped. Standalone files were sent
directly to Decoder. Container tests used Opener only to obtain raw resource
bytes. The test code defines the font variants and optional sample inputs.

## Optional Sample Variables

| Variable | Sample |
|---|---|
| `PEARE_DOOM_IWADS_ZIP` | Path to an external Doom IWAD ZIP archive. |
| `PEARE_RAR4_SAMPLE_ROOT` | External RAR4 and RAR5 samples for the Opener ABI tests. |
| `PEARE_RAR5_RECOVERY_SAMPLE` | External RAR5 archive with an inline recovery record. |
| `PEARE_RAR5_RECOVERY_EXPECTED` | Original payload stored in the RAR5 recovery sample. |
| `PEARE_CAB_SAMPLE_ROOT` | External uncompressed, MSZIP, and LZX15 CAB samples for the Opener ABI tests. |
| `PEARE_CAB_QUANTUM_SAMPLE_ROOT` | External CAB samples for Quantum windows 10 to 21, mixed compression methods, and history across blocks. |
| `PEARE_RAR7_SAMPLE` | External RAR7 algorithm version 1 sample for the Opener ABI test. |
| `PEARE_RAR7_VERIFY_FULL` | Set to `1` to decode the full 4.56 GB RAR7 sample and check its output CRC. This test needs several GiB of RAM. |
| `PEARE_LZIP_SAMPLE_ROOT` | Path to the GNU Lzip source `testsuite` directory. |
| `PEARE_LZIP_MULTI_MEMBER_SAMPLE` | Lzip file created by an independent encoder from `test.txt` and `README.md`. |
| `PEARE_LZIP_EMPTY_SAMPLE` | Empty Lzip member created by an independent encoder. |
| `PEARE_LZIP_TAR_SAMPLE` | Lzip file created by an independent encoder from the repository TAR fixture. |
| `PEARE_UUE_RFC_SAMPLE` | External RFC 6873 UUencode Base64 sample with introductory and trailing text. |
| `PEARE_UUE_RFC_PAYLOAD` | Expected payload decoded from the RFC Base64 vector by Python's standard decoder. |
| `PEARE_ZIPX_EXTERNAL_SAMPLE` | Optional multi-member PPMd ZIPX archive with reviewed payload hashes. |
| `PEARE_ZIPX_PPMD_SAMPLE` | Optional 7-Zip PPMd ZIPX sample created from the project README. |
| `PEARE_ZIPX_WAVPACK_SAMPLE` | External ZIPX archive with WavPack-compressed WAV members. |
| `PEARE_ZIPX_WAVPACK_ORACLE_DIR` | Original WAV members for the WavPack ZIPX comparison. |
| `PEARE_ZIPX_MP3_SAMPLE` | WinZip ZIPX archive that uses method 94. |
| `PEARE_ZIPX_MP3_SOURCE` | Original MP3 source used for the method 94 comparison. |
| `PEARE_ZIPX_JPEG_SAMPLE` | WinZip ZIPX archive that uses method 96. |
| `PEARE_ZIPX_JPEG_SOURCE_DIR` | Original JPEG source directory for the method 96 comparison. |
| `PEARE_ZSTD_ERROR_SAMPLE_ROOT` | Optional upstream Zstandard decompression-error corpus for malformed-stream rejection. |
| `PEARE_ZSTD_UPSTREAM_SAMPLE_ROOT` | Optional upstream Zstandard golden-decompression samples. |
| `PEARE_ZSTD_CLI` | Official Zstandard CLI used to provide expected output for upstream samples. |
| `PEARE_ARJ_SAMPLE_ROOT` | Local ARJ archives used by the Opener ABI test. |
| `PEARE_ARJ_ORACLE_ROOT` | Independent 7-Zip extractions used as the expected ARJ output. |
| `PEARE_ARJ_EDGE_SAMPLE_ROOT` | External ARJ32 legacy-encrypted sample root. |
| `PEARE_ARJ_ENCRYPTED_SAMPLE` | Optional external ARJ32 encrypted sample. |
| `PEARE_ARJ_ENCRYPTED_ORACLE` | ARJ32 extraction of the encrypted sample. |
| `PEARE_ARJ_GOST40_SAMPLE` | Optional external ARJ32 GOST40-encrypted sample. |
| `PEARE_ARJ_GOST40_ORACLE` | ARJ32 extraction of the GOST40 sample. |
| `PEARE_ARJ_GOST256_SAMPLE` | ARJ version 2 archive encrypted with GOST256. |
| `PEARE_ARJ_GOST256_PASSWORD` | Password for the ARJ version 2 sample. |
| `PEARE_ARJ_GOST256_ORACLE` | ARJ extraction of the GOST256 sample. |
| `PEARE_ARJ_GOST256L_SAMPLE` | ARJ version 3 archive encrypted with GOST256L. |
| `PEARE_ARJ_GOST256L_PASSWORD` | Password for the ARJ version 3 sample. |
| `PEARE_ARJ_GOST256L_ORACLE` | ARJ extraction of the GOST256L sample. |
| `PEARE_ARJ_GOST256L64_SAMPLE` | ARJ version 3 sample with a 64-byte password. |
| `PEARE_ARJ_GOST256L64_PASSWORD` | 64-byte password for the GOST256L sample. |
| `PEARE_ARJ_GOST256L64_ORACLE` | ARJ extraction of the 64-byte-password sample. |
| `PEARE_ARJ_SPLIT_SAMPLE` | External multi-volume ARJ archive used by the split-member ABI test. |
| `PEARE_ARJ_SPLIT_ORACLE` | ARJ extraction of the complete split-member sample. |
| `PEARE_FONT_SAMPLE_ROOTS` | Optional list of font directories, separated by the platform path separator. |
| `PEARE_NTFS_TEST_IMAGE` | Disposable copy source for an NTFS image. |
| `PEARE_VMDK_STREAM_TEST_IMAGE` | Stream-optimized VMDK sample. |
| `PEARE_UNSAFE_VMDK_TEST_IMAGE` | Split VMDK with divergent redundant grain-table entries; read-only Writer target probe only. |
| `PEARE_VMDK_TEST_IMAGE` | Monolithic sparse VMDK sample. |
| `PEARE_VMDK_SPLIT_TEST_IMAGE` | Split VMDK descriptor; its extents are copied with it. |
| `PEARE_VHD_DYNAMIC_TEST_IMAGE` | Dynamic VHD sample. |
| `PEARE_VHDX_TEST_IMAGE` | Lumia VHDX sample. |
| `PEARE_EXT_DISK_TEST_IMAGE` | Real MikroTik CHR disk with Ext2 and Ext3 partitions. |
| `PEARE_EXT4_DEPTH3_TEST_IMAGE` | e2fsprogs Ext4 image with a three-level unwritten-extent tree. |
| `PEARE_EXFAT_TEST_IMAGE` | 64 MiB exFAT sample formatted by `exfatprogs`. |
| `PEARE_QEMU_IMG` | QEMU executable; used to create disposable container variants. |
| `PEARE_FAT16_TEST_IMAGE` | FAT16 image sample. |
| `PEARE_FAT_DOS_TEST_IMAGE` | DOS FAT image sample. |
| `PEARE_FAT_FLOPPY_CUD_IMAGES` | Semicolon-separated floppy image paths. |
| `PEARE_DSK_TEST_IMAGES` | Semicolon-separated OS/2 DSK image paths. |
| `PEARE_VMWARE_VDISKMANAGER` | VMware `vmware-vdiskmanager.exe` path for independent VMDK checks. |

The Lzip sample corpus comes from the [GNU Lzip 1.26 source release](https://download-mirror.savannah.gnu.org/releases/lzip/).
GNU Lzip 1.11 created the local multi-member sample from `test.txt` and `README.md`.
It also created the empty-member sample from an empty input. These samples are not committed.

The DOS FAT12 test uses Peare rollback-journal state fixtures next to a
disposable copy. It checks legacy and checksummed recovery, then rejects
malformed or corrupted journals without changing the image. These fixtures are
not filesystem samples.
