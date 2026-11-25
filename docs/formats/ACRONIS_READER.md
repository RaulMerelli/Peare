# Acronis Reader

TIB and TIBX are different backup container families. The target list is not a
claim that either family is fully supported.

## Verification status

The selected TIB/TIBX ABI and offscreen GUI corpus passed on 2026-11-23:
`360 passed, 8 skipped` in one full run against the local Release build. The
eight skips require optional external tools or images. Repository HEAD was
`47029fef85af63d845a44de2781e45158edc1dba`, but the worktree had local changes;
that commit alone does not identify the exact tested source state.

The sample set verifies specific layouts, not every Acronis release or option.
Items below marked unverified or experimental must not be treated as general
compatibility claims.

## TIBX

A file backup created with Acronis Cyber Protect 15 using archive format 12
and no snapshot opens through the ABI. Its file hash matches the original.
This verifies one small, unencrypted backup, not every format 12 variant.
An independently created format 12 backup of a small FAT16 disk opens through
the ABI; a text file and a complete executable match the source files.
The archived stream omits the first sector of its second FAT. Peare's
reconstructed partition matches the complete original partition. An
Acronis-mounted backup also matches the source at the omitted sector.
The original stream remains available through `peare_resource_read_original`.
For FAT32, the active FAT index is used only when mirroring is disabled.
The inspected backups do not exercise a nonzero index with mirroring enabled.
An independent GPT backup also contains a FAT16 volume whose two complete
tables match when mounted by Acronis. Peare's reconstructed tables match both
mounted copies; the original stream has zeros for the second table.
In a separate FAT32 backup, the first sectors of both FAT copies read through
an Acronis mount are identical and match Peare's reconstructed copies.
An earlier analysis attributed addresses `0x180BBB890` and `0x180BBD0B0` in an
Acronis Cyber Protect 15 `DiskBundle.dll` to FAT16/FAT32 table writers. A
targeted IDA pass against the preserved build 26172 DLL instead places those
addresses inside runtime-function ranges associated with
`HFSPlusJournalCircularBuffer::Pop` and `NTFSProcessor::BlockPlacement`.
The attribution is therefore unverified and must not be used as implementation
evidence. The common-path address `0x180C034D0` is inside a generic dispatcher;
its caller and virtual methods are not yet traced to a FAT operation. No
verified FAT writer or condition selecting the reconstruction path has been
identified from this pass.
In the examined filtered reconstruction, allocated clusters not visited by
the directory scan are marked bad in the generated FAT. The reconstruction
also marks filtered directory entries as deleted in its read-only view.
The examined FAT fix uses a nonempty bitmap to select the filtered path. It
checks directory chains for cycles and shared clusters, and checks file chains
up to the declared file size. Excluded entries are marked deleted in the mounted
view. Without a bitmap, the examined path copies the selected FAT to the other
copies. Peare applies that path to TIBX FAT partitions with no stream 4. The
authentic Acronis 15 sector-by-sector sample has no stream 4 and passes an ABI
comparison of both FATs against its source VHDX. In that sample both copies
already match, so it does not exercise repair of divergent copies.
The independent `t1_disk_mbr.tibx` sample has a preserved Acronis mount oracle.
In `disk1/part2`, sector 7038 is the first FAT32 FAT sector, not the boot
sector. `peare_resource_read_original` returns `0x0f` at byte 3 of that sector;
the reconstructed public view returns `0xff`, matching the saved Acronis
mount sector. The ABI test compares both complete 577-sector FAT copies with
the mount hashes. The existing path is the selector-4 filter through
`AcronisRestoredStore` and `reconstructAcronisFat`, which reconstructs the
reserved FAT entries and mirrors the active FAT. This verifies the sample,
not a general boot-sector adjustment rule or every FAT32 layout. A prior note
attributed a parameter-dependent boot-sector update to Acronis FAT code, but
the targeted build 26172 trace did not validate that path.
Peare builds its filtered FAT view on the first partition read. That operation
scans the selected directory tree and may be slow or fail on an unreadable
directory even when the requested bytes are elsewhere. Directory navigation
uses the original stream and reads directories on demand.
Concurrent original and reconstructed reads were checked on the format 11
FAT16 backup, including concurrent initialization of the reconstructed view.
FAT detection also accepts a structurally valid BPB without a filesystem-type
label. An independent unlabeled FAT12 disk was checked through the ABI against
7-Zip. An Acronis backup of an unlabeled FAT volume has not yet been checked.
For the examined TIBX writer and reader, metadata tag 90 marks a partition
backed up sector by sector. Its absence does not by itself establish which FAT
reconstruction path applies. This marker has not been verified for legacy TIB.
The examined Archive3 partition-fix method reads stream 4 as a cluster filter
bitmap and passes it to the FAT fix. A set bit can affect directory processing
as well as FAT reconstruction. The conditions under which the mount service
invokes that method have not been established for every backup variant.
Peare applies the bitmap to FAT directory entries using the starting cluster.
On the format 12 FAT16 sample, root and nested entries match Acronis's file
listing. FAT16 and FAT32 root listings in a separate GPT backup also match
Acronis. The FAT16 comparison includes a nested directory with long and
Unicode names. Other bitmap layouts need independent samples.
Linked volume aliases are not hidden when their filter descriptors differ.
A marked, unformatted partition in the independent GPT sample matches a
separate reader across its complete contents. A FAT16 disk backed up sector
by sector with Acronis Cyber Protect 15 also matches the complete source
partition, including both FAT copies.
Another file backup made with maximum compression matches the complete
original file through the ABI.
A full and incremental file-backup chain created by that tool exposes both
recovery points through the ABI. The incremental file matches an Acronis restore.
An AES-256 chain created by the same tool also exposes both expected files;
the incremental file matches an Acronis restore and the offscreen GUI opens
it with the expected hash.
An AES-256 file backup made by the same tool also opens with the correct
password; its nested file matches the original through the ABI. Missing and
incorrect passwords are rejected for that sample. The offscreen GUI also
opens its nested file and matches the original hash.
Two more format 12 file backups created with the tool's AES-128 and AES-192
options also pass the ABI password checks and match the source file hash.
An independent reader identifies their data keys as AES-128-GCM and
AES-192-GCM respectively. Both files also open through the offscreen GUI with
the same hashes. This does not establish every encryption layout.

The in-development reader uses checked pages and reconstructs volume and file
data through the Opener ABI. Real samples cover stored, LZ4 and Zstandard
segments; full, incremental and differential recovery points; split files;
file backups; and password-protected AES-256-CBC and AES-256-GCM archives.
Offscreen GUI tests open nested files in the AES-256-GCM and AES-256-CBC
samples and compare their contents with independent references.
The reader also implements zlib segments, but they have not yet been checked
against an authentic TIBX sample.
Index pages spanning multiple physical pages are also implemented; this layout
has not yet been exercised with an authentic sample.
A format 12 file backup with 1,024 files exercises multiple index leaves and
internal directory pages; all paths and contents match their source files.
An incremental point checks the same tree after deletions, a rename, an
addition and a modification. Its complete Peare tree matches the source;
all 1,022 files restored by Acronis match the source byte for byte.
An authentic Cyber Protect 15.0.30984 format 12 chain with one full and two
incrementals, plus its Acronis-consolidated result, is checked through the
public Opener ABI. The selected payloads in all three chain points and the
single consolidated point match hashes recorded against their source files.
Acronis validates both archives. This is evidence for this retention sample,
not every TIBX consolidation layout.
Strict item-index seeks use the internal page's upper bounds. For repeated
inline keys, the latest cell takes precedence, including deletion markers.
Neither case has yet been verified with a sample containing that layout.
Item-index reads are restricted to the selected recovery point and item type.
The item index uses the archive's plain or hierarchical name ordering; extent
overlaps are resolved by slice and sequence. Two overlapping ranges in an
encrypted incremental backup were checked against an independent reader.
The Acronis `archive3.dll` reader uses the extent's segment index as a
16-byte offset, except for the whole-segment marker. A synthetic ABI test
checks nonconsecutive indices shared by two volumes; the decompiled path was
examined in one Acronis version, not every format variant.
Uncovered logical ranges are returned as zeros. The Acronis reader offers both
zero-filling and hole-reporting read paths; the callers used for every backup
variant have not been identified. A referenced segment that cannot be read is
reported as an error, not treated as a logical hole.
An ABI test using synthetic records covers distinct item slices sharing a
stream ID. This case has not yet been checked in an authentic archive.
Synthetic ABI tests also cover multiple primary-stream descriptor flag values.
File-backed directories are enumerated one level at a time. Other versions,
encryption settings and backup layouts need coverage.
Child directories reuse their opened archive context; independently opened
archives do not share it. Deleted recovery points are excluded from the normal list;
that case has not yet been checked against a sample containing deleted slices.
The filter follows the default Acronis API enumerator. It hides internal
recovery points when `(flags & 0x73) == 3`. Peare retains those points for data
resolution.
No acquired sample exercises this filter. This rule does not establish how
the Acronis GUI presents recovery points.
Data extents are loaded per requested volume or file, rather than for every
item in the archive.
Recently used segment data is cached per archive; segments larger than the
cache budget remain readable and are not retained.
An ABI test reads two volumes of `t1_multivol.tibx` concurrently. It compares
800 positioned reads with full-volume references checked against known hashes.
An ABI test injects a read failure into an NTFS directory index inside a real
TIBX volume and checks that opening the directory fails.
The archive continuation table distinguishes standalone backups from split
backups when similarly named files are present in the same directory.
For a [seven-part archive](https://archive.org/details/win-10-2020-11-28),
callback-backed range reads exposed three volumes. Their first 4 KiB matched
an independent reader. The full archive and volume contents were not checked.
An ABI test also opens all seven parts as separate range-backed sources and
matches the three volume prefixes against independent hashes. This exercises
multipart assembly without downloading the complete parts; it does not verify
all their data or discovery of sibling files by path.
An independent [five-part archive](https://archive.org/details/hp-pro-book-4520s-win-7-hb-x-86-tr)
also opens through the multipart ABI; its first volume prefix matches an
independent hash. Both archives have consecutive parts and do not verify
retention with removed volumes.
Two WOF-compressed NTFS system files in that archive matched an independent
XPRESS-Huffman decompression of their named streams. This check used further
HTTP ranges from the concatenated source.
After the TIBX positioned-read changes, the WOF FILE-provider range test also
passed with an injected backing-source failure.
The same volume contains a WOF file backed by a WIM resource on that volume.
Peare recovered the complete file and matched its hash against an independent
WIM reader. Tests also cover a failed source read and changes to backing data.
An additional range-backed ABI test reads the complete file from the original
backup without modifying the source and verifies the same independent hash.
The same-volume WIM test passed again after enabling strict NTFS path lookup.
Synthetic NTFS/WOF ABI tests also cover external indexes, attribute-list
extensions and invalid or unreadable records.
The legacy TIB reader resolves a WOF backing WIM from sibling partitions. Two
authentic Cyber Protect 15 backups exercise both directions: a GPT backing
WIM with an MBR target, and an MBR backing WIM with a GPT target. All five
files in each target volume match their hashes from the external WIM through
the public Opener ABI. A backing is accepted only when the WIM identity and
referenced resource verify; missing or ambiguous sibling candidates fail
closed. The tests are `test_tib_cross_volume_wof_wim` and
`test_tib_cross_volume_wof_mbr_backing`. Sample provenance and hashes are in
the external sample dossier. Other cross-volume and WOF layouts still need
separate samples before their compatibility can be claimed.
Compacted split-volume layouts and unaligned raw fragments need further format
analysis and samples. Logical section slots for headers older than version 8
follow the independent reader's mapping but lack an authentic pre-8 sample.
Recovery-point metadata is not currently exposed before password validation.
The reader accepts RSA private keys in PEM PKCS#1 or PKCS#8 format through
the Opener ABI. Encrypted PKCS#8 keys using PBES2, PBKDF2 and AES-CBC are also
supported; their passphrase is separate from the backup password. A format 12
backup with a public-key record
added by Acronis Cyber Protect 15 opens without the backup password; its file
matches the original. The independent test identified RSA PKCS#1 v1.5 key
wrapping. The same archive also has a password record. A separate copy whose
password record was removed by Acronis opens with the private key and yields
the same file hash. Offscreen GUI tests also exercise both credential choices,
incorrect credentials, retries and directory navigation on these backups.
Other encrypted key formats, RSA padding schemes and key types have not been
verified. The RSA implementation uses base and exponent blinding, but its
arithmetic and padding checks are not constant-time; it
must not be exposed as a remote decryption oracle.
Unlocking also tries each distinct matching RSA record, so an archive with
many such records can take a long time to open.
The Acronis engine also attempts to select Grasshopper-CBC for `gost2015`.
Peare implements this cipher for password-wrapped keys; ABI tests use synthetic
archives and the standard block-cipher vector. No authentic `gost2015` archive
has verified interoperability. CBC does not authenticate the data. Synthetic
key-unwrapping tests also cover passwords of 64, 65 and 80 bytes.

Other samples come from the [dissect.archive TIBX tests](https://github.com/fox-it/dissect.archive/tree/main/tests/_data/tibx).
The source describes identity-field anonymization and the patched compression
setting in two True Image 2020 archives. The independent reader was used to
compare complete volume hashes and file contents.

## TIB

True Image 8.0 build 764's filesystem-mode v1 sample
(`trueimage8-en-764-demo.tib`, header `0x8F5C36C6`) opens through the public
ABI. The reader validates the catalog bounds and records and exposes the raw
disk and partition catalog records; this sample yields two disk records and
five partition records. It does not map partition payloads or reconstruct
filesystems. This evidence does not establish support for other 8F variants.

A file backup created with Acronis Cyber Protect 15 using archive format 11
and no snapshot opens through the ABI. Its file hash matches the original.
This verifies one small, unencrypted backup, not every format 11 variant.
An independently created format 11 backup of a small FAT16 disk opens through
the ABI; a text file and a complete executable match the source files.
The archived stream omits the first sector of its second FAT. The reconstructed
partition matches the complete source partition; the corresponding sector also
matches the Acronis-mounted TIB. The source partition hash was read through
Peare's VHDX reader; the full Acronis-mounted partition has not yet been hashed
independently. The original stream remains available through
`peare_resource_read_original`.
For this indexed disk backup, Peare applies the recorded cluster bitmap to
FAT directory entries. The root and nested listings match Acronis, and the
offscreen GUI opens a nested file with the expected hash. A failed bitmap
read is reported as an error. Bitmap addressing in multipart legacy disk
backups has not yet been independently verified.
Peare also checks reads beyond the bits used by the FAT geometry. An authentic
backup with such a bitmap has not yet been verified.
Indexed metadata and sector-map headers also accept extended field lengths
found in the Acronis reader; an authentic archive using this encoding has not
yet been verified.
A separate format 11 file backup made with maximum compression also matches
the original file through the ABI.
A two-file full and incremental backup created by that tool opens from each
file as its respective recovery point. Both payloads match their originals,
and the incremental payload also matches an Acronis restore. The offscreen
GUI opens both points; a missing base file is rejected.
An AES-256 chain also opens at each point; its incremental payload matches
an Acronis restore and the offscreen GUI opens it with the expected hash.
A corrupted incremental index is rejected instead of
silently returning the earlier point; callback tests also corrupt each of
the two metadata records and require an error.
A separate six-point chain covers file addition, replacement, deletion,
recreation and directory deletion. The incremental trees and file hashes
match Acronis restores for the checked points. Other incremental layouts
remain to be verified.
An AES-256 two-point file chain also verifies a deleted file against an
Acronis restore.

Peare currently routes the `0xA2B924CE` header with version 1 to the Mac path
and expects a `.metadata` companion. This is not established as a unique Mac
identifier: the independent [very-legacy analysis](https://github.com/TreadingTheTiber/acronis-tib-reader/blob/master/docs/FORMAT_VERY_LEGACY.md)
reports a Windows migration path gated by the same magic and version, plus
flags `1` and sector size `0x1000`. That analysis has no authentic sample and
infers the product era, so this Windows subtype remains unverified. Peare must
not claim it is supported until a real sample or independently traced writer
confirms the complete header and payload layout. The Mac parser can inspect its
common header and companion supplied beside a file or through the multipart
source API.
An experimental parser exposes slices and depth-first directory entries, and
reads ordered data handles lazily. It checks the proposed start-minus-five box
relation and validates each complete zlib stream before returning bytes.
No authentic Mac sample has verified this interpretation. Directory counts and
slice inheritance are not established as universal; the handle record-size
field's meaning is unresolved.

The in-development reader opens the uncompressed, disk-image variant found in
a True Image 11 backup. It reads checksummed blocks through positioned stores,
validates the block maps and matches the recovered streams to MBR partitions.
File hashes were compared with an independently produced Ghost backup of the
same source disk, and both partitions were opened through the ABI and GUI.
Treating the embedded FAT volumes as contiguous images produces incorrect
file data. Sparse blocks, other compression modes and other TIB versions still
need independent samples and verification.

The sample is published by the [Internet Archive contributor](https://archive.org/details/lte-elite-475-cx).
Observations from one sample do not define other TIB versions or compression
and encryption variants.
Classic TIB file-level backups use different records from disk-image backups.
An older file-level layout described by
[`tib-recover`](https://github.com/elsbrock/tib-recover) uses compressed
fields and catalogs across numbered volumes. Peare can open file backups
created by True Image Home 11.0.8053. Tests cover a full backup
with nested folders, an empty file and a binary file, and a subsequent
incremental backup with an added file and a deleted file. Extracted bytes match
the original files. The incremental directory listing matches Acronis for
the tested entries. A separate full backup split across three volumes opens
from each part, and its binary matches the original. The reader rejects that
sample when its middle part is missing. Its catalog parsing remains heuristic;
other versions and split layouts require separate tests. A password-protected
True Image 11 sample contains readable file data, but Peare does not enforce
the password that Acronis requires before restoring it. Password validation for
this historical layout remains unresolved.
An empty candidate scan is rejected; it does not establish an empty catalog.
The independent reader's observed catalog starts with four little-endian words
`1, 2, 1, 1` in a 32-byte header. The True Image 11 samples start their
catalogs with `1, 1`; their entries begin after the initial words. The parser
scans on two-byte boundaries for two
length-prefixed UTF-16 strings followed by at least 36 metadata bytes. The
64-bit value 12 bytes into that metadata references a previously checked
`0x66` field. Candidates are traversed in reverse order, using the referenced
field's depth and type to construct the tree. A snapshot requires a catalog in
a section closed by `0x68`; an incomplete trailing section does not replace it.
The reference rejects multiple catalogs in one section; the experimental index
does the same. This restriction is not established for every TIB variant.
See the [reference catalog reader](https://github.com/elsbrock/tib-recover/blob/fa7fd65734247c059837b849433edb80164db5d3/incremental.go).
Its 36-to-4096-byte metadata-span check is a plausibility rule, not a proven
record boundary. Neither the remaining header words nor all record fields are
understood. These observations do not establish a general TIB version or prove
that scanning finds every catalog entry.
The referenced path field stores a UTF-16 name, a one-byte item type, and a
little-endian depth. Logical offsets exclude volume headers that continue a
field, but count the header at each new section. Catalog names can differ from
the referenced path names after a rename. The observed file envelope starts
with version words `3, 2`; its first stream of type `1` holds the primary
content. Other stream semantics and other envelope versions remain unverified.
Field data is tentative until its stored Adler-32 or CRC-32 has been checked.
When a recognized footer is present, it reads volume lengths from the footer;
otherwise it uses file length minus the 12-byte header. The footer layout was
checked against an authentic single-volume sector backup and a three-volume
True Image 11 file backup.
True Image 11's volume reader compares the second header word against the
initial volume when opening each requested volume. It also checks the first
word and volume sequence. The experimental field scanner applies these checks;
matching headers alone do not prove that a file-level backup is complete.
Some share backups use sequential `m` content records terminated by `n`.
Peare does not yet resolve those records to their original files. Matching
files only by size is ambiguous and is not used.
For metadata versions below 4, synthetic ABI tests exercise file and directory
entries, sparse data, special Unix item metadata and malformed records. These
tests do not replace verification against an authentic legacy file backup.
The metadata 3.1 tree is also verified against a full, unencrypted file-level
backup created by Acronis True Image Home 2009 build 9769. The public ABI lists
`C:/test/new.txt`, `C:/test/test.txt` and `C:/test/sub/cmd.exe`, and reads their
payloads. This sample exposed directory type `fieldE=128`, which the legacy
tree reader now recognizes. Its original guest file hashes were not captured
before backup, so the ABI digests verify stable sample extraction, not an
independent pre-backup comparison. Sample provenance and the regression test
are kept outside Git and in `tests/test_tib_abi.py`, respectively.
The Mac file-level variant uses a separate `.tib.metadata` companion and is
not implemented by the Windows file-level reader. No accessible Mac sample
has yet been obtained for verification.
Three [Windows file-backup samples](https://github.com/hmaerki/fork_acronis-tib/tree/ac79fbfb4c7b97db1fd245c01f2d4aba342817e3/sample_tib)
include an empty file without a block map. Peare enumerates both files and
reads the available compressed data through the ABI and offscreen GUI. One
block is one byte shorter than its declared logical length; Peare pads that
byte with zero. For the first sample, a file restored by Acronis Cyber Protect
15 matches Peare's complete file hash, including that byte.
The short block's recorded MD5 matches the decompressed bytes before padding.
Static analysis of an Acronis reader found a zero-fill path for successful
short reads from direct `m` blocks; the exact runtime path was not traced.
The record reader accepts zlib headers with smaller valid window sizes, but
no acquired TIB sample has exercised those headers.
For metadata version 5.2, the no-map fallback uses the size field at `T+12`.
The field matches the file size in these samples, but not all 5.2 items have
been checked. A present block map is parsed even when the primary size is zero.
No acquired sample verifies that combination with an alternate stream.
One [file-level sample](https://www.reddit.com/r/acronis/comments/14gc810/)
opens with its directory hierarchy, primary file and alternate stream. Peare
matches the reported all-zero primary file and the alternate stream's bytes.
The author reports an Acronis restore issue for this sample. Other file-level
layouts, compression and encryption options, and incremental chains still
need independent samples and verification. Its block map has no block hash.
Direct block maps containing hashes have been checked against three file-level
samples. Peare reads provider-backed deduplicated file blocks from a local
`unified_data.ds.0` or `.ds.1` store alongside the archive or in an ancestor
vault directory. When present, the corresponding `.szh` file supplies the used
store length; otherwise Peare uses the physical file length, as the 11.5
reader does. Record validation still applies to the entire used range.
The reader checks the decoded block against its MD5 key and rejects missing,
truncated, or corrupt records. An Acronis Cyber Protect 15 format-11 file
backup with a deduplicated 11.7 vault has been restored with Acronis and read
through the Peare ABI; all four recovered files match their source SHA-256
hashes. A separate Acronis backup with maximum compression exercises compressed
store records; its restored file matches Peare's output and the source hash.
Damaged stores have additional synthetic tests. An Acronis format-11
disk backup in an 11.7 deduplicated vault also opens through the ABI and GUI;
its four test files match the source, and Acronis independently restored one
of them. An AES-128 encrypted deduplicated file backup and its store also open
through the ABI and offscreen GUI; recovered files match the source, and a
missing password, an incorrect password and damaged store records are rejected.
An offscreen GUI test also enters the vault password in the password dialog
and checks the recovered file's hash.
Another independently created AES-128 vault backup with maximum compression
contains encrypted compressed records; Peare's recovered file matches the
source hash.
For this layout, vault metadata contains the wrapped key, encrypted store
records have a 41-byte footer, and the encrypted stream stores its IV and
length information separately from the ciphertext. The key and decrypted block
digest are checked before data is returned. The archive trailer position is
read from its footer, not inferred from a fixed distance from the file end.
These observations also match the storage server's block decryption and stream
initialization paths inspected in `StorageServer.exe` from Acronis Cyber Protect
15 build 30984, at image addresses `0x1405ABED0` and `0x140B331E0`.
Other encrypted vault layouts, encryption modes, and provider implementations
remain unverified and are not covered by this result. The
corresponding block data may be held outside the TIB in an Acronis
[deduplication data store](https://www.acronis.com/en/support/documentation/ABR11.5_tracking/3353.html).
Such a backup needs that store as well as the TIB to recover its data.
In Acronis Backup Advanced 11.5, the store uses `.ds.0` or `.ds.1` files;
the deduplication database can be stored separately as a `.db3` file.
Static analysis of Acronis Backup & Recovery 11.5 StorageServer build 38350
found separate disk and file store constructors at `0x1404DF650` and
`0x1405362A0`. They pass `disk_` and `file_` to a shared constructor at
`0x1405AB040`, which appends `data.ds`. The filename function `0x140566F80`
appends a period and the store number; `0x140558690` appends `.szh` for its
size file. The 11.5 size reader at `0x14055AC40` reads eight bytes and caps a
larger recorded size at the data file's capacity. Peare applies the same cap
and reads those first eight bytes without requiring the size file to end there;
invalid or truncated records still fail validation. Peare searches the
corresponding `disk_data.ds.0` and `.1` for
disk backups, and `file_data.ds.0` and `.1` for file backups. Tests exercise
these names with 11.7 or synthetic records, not with an authentic 11.5 vault.
The 11.5 record layout remains unverified. Unusable store groups do not block
reads from a separate valid group. If multiple usable groups contain the same
hash, each matching record must decode correctly and produce identical data.
The file-level index parser handles metadata version 5.2 and the tested 3.1
legacy tree; other legacy versions remain experimental. In particular, 5.0
and 5.1 layouts are not verified and are not enabled by the parser. It detects items that
require a deduplication provider rather than returning invented zero data.
Files that need an unavailable provider cannot be opened.
In the Acronis reader, block-map hashes identify entries requested from a
deduplication provider; the inspected lookup does not itself establish that
the returned data has been verified against the hash.
The inspected DiskBundle client requests items by hash and checks response
cardinality; a decompression wrapper processes the returned items. These
client paths alone do not establish the storage index or complete protocol.
The authentic legacy TIB and matching data store verify the 33-byte footer
path described above, but not all provider and store variants. A missing or
failed provider response must not be treated as zero-filled data.
For encrypted file-level backups, an experimental AES-CBC path derives the
key and checks the decrypted index before exposing entries. Format 11
AES-128, AES-192 and AES-256 file backups created with Acronis Cyber Protect 15 open
with the correct password and their payloads match the original through the ABI.
Missing and incorrect passwords are rejected. This does not establish support
for every encrypted file-backup variant. The offscreen GUI also opens the
AES-256 sample's nested file and matches the original hash.
The same reader also opens Home 2009 build 9769 file-level metadata version 3.1
encrypted with AES-128, AES-192, and AES-256. Public ABI tests compare each
sample's paths, sizes, and payload hashes with the matching unencrypted backup.
This verifies these three Home 2009 samples only; the guest source files were
not hashed before backup, and no claim is made for other legacy versions or
encryption layouts.
Two additional True Image 21 samples, with normal and maximum compression,
also passed the incorrect-password check; their passwords are not known.
Static analysis found positioned AES-CBC reads that use the preceding source
block as IV. The source stream's physical origin has not been established.
Additional stream wrappers and encryption options may need support.
For TIB volumes with a numeric suffix, Peare can discover sibling parts and
assemble their validated logical ranges. A missing-part case was checked with an independent
final volume. A three-part file backup created with Acronis Cyber Protect 15
opens from each part and its complete file matches the original through the
ABI; a missing middle part is rejected. This sample has an unnumbered first
part and a longer file-index trailer. It does not establish support for all
split layouts. A separate three-part AES-256 backup made with the same tool
opens from each part and its file matches the original through the ABI.
For both backups, the offscreen GUI opens the file's PE headers and matches
their hash against the original file.
The multipart ABI also opens a four-part [Packard Bell disk backup](https://archive.org/details/PackardBell_Dot_SE_572RU).
Its first block and one block crossing each of the three volume boundaries
matched an independent reconstruction. Missing and out-of-order parts were
rejected.
The rest of the backup was not fully verified.
For the modern TIB header, Peare verifies the stored Adler-32 after applying
the format's default data-offset rule. The reconstructed final-volume footer
is checked separately.
Repeated reads of a file-level data block reuse the most recently decoded
block when it fits in the cache. Larger blocks remain readable without being
retained. The alternate stream in the sample was checked with concurrent,
positioned reads through the ABI.
An injected source failure during a file-level read of an Acronis-created
split backup returns an error without changing the caller's buffer.

Another legacy disk backup, published in the [Xandros recovery collection](https://archive.org/details/xandros-linux-asus-eee-pc-l701_ru_1.0.6.6.),
uses Zlib-compressed bitmap blocks and contains four partitions. Peare opens
the two Ext volumes and an empty FAT12 volume; files and the FAT boot sector
match independent readers. The fourth partition is not identified, but its
complete recovered byte stream matches an independent reconstruction. The
FAT12 volume is recognized from its readable boot sector even when a longer
header read crosses its recorded readable limit. FAT metadata read errors are
reported instead of being interpreted as an empty directory.
The map's leading-sector prefix was verified on this FAT12 sample and on a FAT16
volume from another backup. The complete Ext volumes and unrecorded
partition tails still need verification.

The indexed, compressed legacy format also opens the
[Sony VAIO recovery backup](https://archive.org/details/sony-vaio-ngn-ux-380-n-recovery-partition)
through a range-backed ABI source. Its first 256 KiB matched an independent
decompression; the rest of the archive has not been fully verified.

A second reader path opens the compressed sector stream in the
[Windows 2000 backup](https://archive.org/details/win2k-sp4). Its indexed
blocks use a 64-cluster bitmap and zlib. The NTFS contents were compared with
an independent NTFS reader over a separately reconstructed stream; the TIB
block reconstruction itself does not yet have an independent implementation
that opens this sample. This sample's block map ends before the NTFS volume
length: reads beyond the map fail instead of returning invented bytes. A
separate FAT16 sample also verifies a different sector-stream geometry. Map
fragments, other geometries and other compression methods remain open.
The sector reader caches decompressed blocks; nested NTFS directory lookups
also reuse directory indexes within an open reader.
Compressed blocks are read incrementally within their recorded boundaries;
their decompressed size and zlib checksum are verified before use.
Positioned reads do not modify the destination if a required block fails.
The uncompressed legacy volume path has the same behavior. An ABI test injects
a source failure after a complete block from a real backup has been read.
For sector maps marked with the D4 deduplication mode, inline blocks can be
read even when the same chunk also contains external references. The reader
handles zlib and uncompressed chunks with Adler-32 checks and resolves external
references through the local deduplication provider. An Acronis 15 NTFS disk
backup produced with the chunk-level flag forced to zero exercises this path
through the public ABI. Acronis restored four files from the same backup, and
their hashes match Peare's results. A missing
or corrupt external reference fails reads that touch it without blocking inline
data elsewhere in the same chunk; synthetic tests cover both error cases and a
valid inline-to-external boundary. They also check multiple references where a
later missing or corrupt block must not reuse previously decoded data. A
synthetic D4 map around an independently created FAT12 image also verifies
offscreen GUI navigation and file content.
Inline sector groups retain only present clusters in the cache. A synthetic
D4 group with a large logical extent and two present clusters is read in a
memory-limited child process; it is not an authentic D4 backup.
An external boot block is resolved before probing the volume in a synthetic ABI
test; missing or corrupted boot blocks are rejected.
For indexed F7 sector maps, the reader resolves external chunks through the
local deduplication store and verifies their MD5 keys before reconstructing
the sector bitmap and data. One genuine Acronis format-11 NTFS disk backup
and its matching 11.7 store exercise this path. A missing store or a corrupt
referenced chunk causes affected reads to fail. With a corrupted data chunk in
a copy of that backup, a separate boot-sector read still matches the unmodified
backup before and after the failed file read. The reader validates an external
record before allocating its expanded group; a synthetic large-group test
covers a missing reference with and without a store. External groups retain
their compact bitmap and stored clusters in the cache; a memory-limited child
process verifies reads from a large sparse synthetic group. Synthetic tests also
exercise mixed inline/external groups and distinguish an empty valid bitmap
from invalid bitmap bits or inconsistent payload lengths. Other F7
configurations and other D4 variants still need authentic samples.
Sector maps are decompressed from the source incrementally. Their expanded
records remain in memory; maps larger than four million records have not yet
been tested with a real backup.
When an indexed volume records a map size, the reader bounds decompression to
that size and verifies exact consumption. Win2k and Xandros samples exercise
this descriptor; synthetic tests cover shorter and longer declared sizes.
For indexed maps with a recorded size, block references need not end at the
start of the map. This follows the inspected Acronis reader; no compact indexed
sample with that layout has yet been verified.
The Acronis reader uses a separate indexed stream for maps marked for random
access. Peare now has a path for indexed pages and bitmap-based groups with
zlib or uncompressed data. It supports the examined ordinary and external
group references. Its stream origin and behavior on encrypted or multivolume
archives are not established. No authentic random-access sample has verified
this path; existing TIB tests do not cover it.
The index root is selected when the stream opens and remains fixed for later
reads, following the examined Acronis initialization path.
The examined Acronis path treats a group containing only a bitmap as empty,
including when that bitmap contains set bits; Peare follows that behavior.
An injected read error at the indexed descriptor of the format 11 FAT16 sample
is reported as an open failure rather than a successful fallback.
Indexed volume metadata can separately record a readable-sector limit. Peare
retains the declared partition size but rejects reads past that limit. The
limit comes from analysis of the Acronis reader; boundary tests use synthetic
archives. Real Xandros records contain different values for these two fields.
Peare also implements extended 20-byte sequential map records and accepts an
uncompressed map payload with an Adler-32 checksum. The uncompressed map
decoder is exercised by the LTE Elite backup; an indexed backup using that
variant and an authentic extended map have not yet been verified.
For overlapping extended chunks, the later chunk takes precedence, following
the inspected Acronis read path. Extended-map discovery requires matching
volume metadata; the handling of multiple extended maps is not established.
The single-volume fallback also reads independently encoded map fragments
whose recorded blocks are physically ordered and non-overlapping, and accepts
a complete final map. Synthetic ABI tests cover both layouts; a real fragmented
backup is still needed. The fallback does not establish ownership of each map;
an unrelated compatible map could be combined with the intended volume.
Block references in real indexed backups can be out of
physical order; fragmented indexed maps are not yet reconstructed.
For a synthetic indexed layout whose locator points only to the final fragment,
the current reader can expose that fragment at logical offset zero. This
layout has not been established in an authentic backup.
The fallback searches for structurally valid map TLVs without requiring a
particular tag order or canonical integer width. The search starts at the end
of the archive; earlier fragments are scanned only when the final map needs them.
Synthetic ABI tests cover reordered fields and scan-window boundaries.
Additional archives inspected by byte range contain backward references in
their block maps. A multi-partition sector-mode backup exposed two NTFS volumes
and a FAT16 volume; files from all three matched independent reconstructions
through the ABI using a temporary range-backed source. The complete archives
have not yet been downloaded or verified end-to-end.
In the Windows 2000 sample, the trailer's `0x80` value equals the total bytes
in stored clusters, not the NTFS volume size. It cannot be used as the volume
capacity without further format evidence.
The sector reader can obtain the volume size from a metadata record linked to
the block map, independently of the filesystem. The Windows 2000, Windows 8.1
and multi-partition samples were verified with this metadata path. Other
filesystem layouts still need real samples and end-to-end verification; an
NTFS boot-sector fallback remains for archives whose metadata layout is not
recognized.
