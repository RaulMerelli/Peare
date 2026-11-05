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
| `tests/test_formats.py` | Opens each eligible repository fixture through the Opener ABI and compares its resource tree with a reviewed JSON manifest. |
| `tests/test_font_samples_abi.py` | Sends standalone font bytes and raw container-resource payloads to Decoder, checks collection faces, scans configured font directories, and tests GZIP/ZIP-wrapped fonts. Opener is tested only for generic container extraction and for not classifying standalone fonts. |
| `tests/test_pfm_samples_abi.py` | Checks real PFM metric files through Decoder text and metadata ABI calls. |
| `tests/test_zip_implode_abi.py` | Compares all members of a real Implode ZIP with 7-Zip through the Opener ABI. |
| `tests/test_wad_decode_abi.py` | Opens and decodes sprites and textures from the external Doom IWAD archive when available. |
| `tests/test_uue_samples_abi.py` | Opens upstream libarchive UUencode samples through the Opener ABI and compares each decoded payload with its paired `.Z` sample. |
| `tests/test_lha_samples_abi.py` | Opens configured LHA samples through the Opener ABI and compares names and payloads with 7-Zip. |
| `tests/test_7z_methods_abi.py` | Compares 7z codecs, filters, headers, six compression levels, coder properties, and solid-block settings through the Opener ABI with 7-Zip 19.00. Set `PEARE_7Z_REAL_SAMPLE_ROOT` to compare an external solid archive corpus with 7-Zip. |
| `tests/test_writer.py` | Runs CUD operations on disposable disk-image copies and checks supported Peare-created VMDKs with VMware when configured. |
| `tests/test_android_package.py` | Checks the ELF architecture of native libraries in a built Android APK. It does not run the APK on a device. |
| `tests/run_tests.py` | Runs pytest and stores pytest files and Python bytecode in `tmp/test-output`. |
| `tests/run_gui_preview_tests.py` | Runs offscreen GUI previews for arbitrary source files and full resource-tree paths. |
| `tests/run_gui_writer_tests.py` | Runs a manifest-driven CUD scenario through the GUI on a disposable image copy. |

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
cross any number of nested containers and can select a container or a leaf.
The runner saves one screenshot per case and fails if Peare cannot navigate to
the path, render preview content, or save a valid PNG.

On Windows, the build script stores the offscreen plugin in
`build-windows/test-plugins/platforms`. The GUI runners use this path by
default. Set `QT_QPA_PLATFORM_PLUGIN_PATH` to override it.

For a large sample set, pass a JSON manifest. Each case needs `name`, `source`,
and `tree_path`. Set `min_images` when the case must render a known number of
preview images. Use `sample_id` to group several resources from one source file.
Set `--jobs` to run cases in parallel and `--report` to save all results.

```powershell
py tests/run_gui_preview_tests.py `
  --exe build-windows/Release/Peare.exe `
  --case siemens-fwf "C:/samples/mobile.zip" "Mobile2ndGen/17.0/Images/KTP_4_Mobile/KTP_4_Mobile_V17_0.fwf" `
  --case standalone-font "C:/samples/font.ttf" "" `
  --case dll-font "C:/samples/fonts.dll" ".rsrc/RT_FONT/#1001/#1033"
```

The runner does not infer a format or choose a resource. Tests must provide the
same full tree path that a user can select in the GUI. Standalone font files go
directly to Decoder. Opener is used only when the source is a container; it
returns the selected record bytes without interpreting them. A complete font grid requires one displayed
cell per glyph. A blank glyph is valid and does not need visible pixels.

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
| `PEARE_RAR4_SAMPLE_ROOT` | External RAR4 samples for the Opener ABI tests. |
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

The DOS FAT12 test uses Peare rollback-journal state fixtures next to a
disposable copy. It checks legacy and checksummed recovery, then rejects
malformed or corrupted journals without changing the image. These fixtures are
not filesystem samples.
