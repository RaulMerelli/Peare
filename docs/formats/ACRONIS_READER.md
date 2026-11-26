# Acronis TIB and TIBX Reader

This page describes the TIB and TIBX read paths and the sample evidence for
them. TIB and TIBX are separate container families; support for one does not
imply support for the other. Peare does not write either format.

The tables distinguish implemented code from behavior checked with authentic
samples. Synthetic ABI tests cover structures and error handling, not Acronis
interoperability.

## Verified Variants

| Family | Variant | Verified behavior | Evidence and limits |
|---|---|---|---|
| TIB | True Image 8 filesystem-mode v1 (`0x8F5C36C6`) | Reads the disk and partition catalog records. | Build 764 sample. Does not expose partition payloads or reconstruct filesystems; other 8F variants are not covered. |
| TIB | True Image 9.0 builds 2273 and 3677, file backups | Lists paths and reads file data from password-protected samples. | ABI tests compare recovered files with recorded SHA-256 values. |
| TIB | True Image Home 2009 build 9769, file metadata 3.1 | Lists paths and reads file data, including AES-128, AES-192, and AES-256 samples. | Encrypted payloads match the corresponding unencrypted backup payloads; original guest-file hashes were not recorded. |
| TIB | True Image 11, file backups | Reads legacy file trees, full and incremental points, split volumes, and tested AES-256 chains. | ABI tests compare selected payloads with expected bytes or Acronis restores. Other legacy trees remain unverified. |
| TIB | Cyber Protect 15, archive format 11 and file metadata 5.2 | Reads full and incremental file backups, split volumes, encrypted samples, and selected deduplicated backups. | Checked payloads match source files or Acronis restores. Provider-backed blocks require the matching external store. |
| TIB | Disk and partition backups | Reads tested uncompressed and indexed sector maps, bitmap groups, zlib data, and D4/F7 external references. | Authentic samples include True Image 11, Windows 2000, Xandros, and Cyber Protect 15. Each sample covers only its recorded layout. |
| TIB | WOF-backed files | Reconstructs tested WOF files from same-volume or sibling-partition WIM resources. | Authentic samples cover both MBR/GPT directions. The WIM identity and resource must match; absent or ambiguous backing data fails closed. |
| TIBX | Archive format 12 file backups | Reads tested stored, LZ4, and Zstandard data; full, incremental, and differential points; split files; and password-protected AES samples. | Authentic Cyber Protect 15 samples. Coverage includes AES-128/192/256-GCM, AES-256-CBC, and an RSA-wrapped key case. It does not cover every combination. |
| TIBX | Archive format 12 disk backups | Reads tested MBR/GPT images, FAT16/FAT32 volumes, partition data, and WOF-backed files. | Authentic samples were compared with original disks, Acronis mounts/restores, or an independent reader. FAT reconstruction is verified only for the exercised layouts. |
| TIBX | Split and ranged sources | Assembles tested consecutive parts and supports positioned reads from callback-backed sources. | Some large public archives were checked only for volume discovery and initial byte ranges; this is not full-content verification. |

## Implemented, Not Verified

The code paths below exist, but the available evidence does not justify a
general compatibility claim.

| Area | Current evidence | Missing evidence |
|---|---|---|
| TIB file metadata schema 5.0 and 5.1 | The parser accepts major version 5, minor versions 0 through 2, and handles the version-specific fields. | The ABI tests do not assert 5.0 or 5.1 against a dedicated authentic sample. Parser acceptance alone is not interoperability evidence. |
| Legacy TIB file-tree records | The reader contains a legacy parser; ABI tests cover selected True Image 11 samples. | The True Image 11 tree parser remains heuristic. Other legacy layouts need authentic samples and payload comparisons. |
| TIB Mac file backup (`0xA2B924CE`) | Header and companion metadata can be inspected; a file-tree parser is experimental. | An authentic Mac backup and a confirmed writer/layout. |
| TIBX zlib segments | Decoder path is implemented. | An authentic TIBX sample using zlib segments. |
| TIBX item-index page spanning multiple physical pages | Page handling is implemented; other multi-page index structures have sample coverage. | An authentic sample exercising this exact page layout. |
| TIB random-access and extended sector maps | Selected map paths and synthetic layouts are implemented. | Authentic samples for random-access maps, extended records, and fragmented or out-of-order maps. |
| Uncommon recovery-point and stream descriptors | Synthetic ABI cases cover selected flags, slices, overlaps, and holes. | Authentic archives exercising those exact combinations. |
| Other Acronis releases, ciphers, vaults, and split layouts | Behavior has been inspected or tested in selected builds only. | A sample and independent comparison for each claimed variant. |

## Known Limits

- A legacy file backup that references an external deduplication provider
  requires the matching data store. A missing, corrupt, or ambiguous block is
  an error; Peare does not substitute zero-filled data.
- The tested deduplication-store layout uses `.ds.0` or `.ds.1`; an optional
  `.szh` file supplies the used store length. Other store versions are not
  established by these samples.
- TIBX FAT reconstruction, D4/F7 maps, WOF resolution, and volume-size
  selection are verified only for the sample layouts listed above. Do not
  generalize those results to every filesystem or archive version.
- Some large multipart archives were tested through ranges and prefixes only.
  Their complete contents and missing-volume retention behavior are not
  verified.
- RSA private-key decryption is not constant-time. Do not expose it as a
  remote decryption service.
- CBC encryption does not authenticate data. A successful decrypt is not by
  itself proof that the archive is intact or authentic.

## Tests

The public ABI tests are in [`test_tib_abi.py`](../../tests/test_tib_abi.py)
and [`test_tibx_abi.py`](../../tests/test_tibx_abi.py). They use authentic
samples when configured and skip cases whose external samples or tools are
unavailable. Synthetic cases test error handling and individual structures;
they are not evidence of Acronis interoperability.

The format matrix lists TIB/TIBX as readable, not writable. See
[`SUPPORTED_FORMATS.md`](../SUPPORTED_FORMATS.md) for the project-wide
declarations.
