# Adding a Writer Format

This guide is for maintainers who add or change a PeareWriter backend.

## Required support

When a request asks for Writer support for a format, it means complete CUD
support. Implement creation, update, and deletion for every advertised writable
variant. Also implement rename and replacement where the format has entries.
Do not treat creation of a new container or file addition as complete support.

Support includes two separate paths:

1. Create a new output from zero, with each supported variant available through
   the generic Writer format and variant queries.
2. Update an existing output with the applicable CUD operations.

For an archive or file system, support file creation from a source file, empty
file creation, directory creation, file replacement, rename, and deletion.
Support recursive directory deletion when the format has directories.
For a flat stream, mark member operations `N/A` only when the format has no
independent members. This does not remove the requirement to create that stream
from zero or update its meaningful contents.

Do not claim `Write: Yes` for a format variant until all applicable operations
work. Do not report partial Writer support as `Yes`.

## Component rules

- Keep the Writer independent from the Opener and Decoder.
- Keep format-specific behavior out of the GUI.
- Expose public functions and data through the C ABI only.
- Keep Qt, C++, STL, and exception types out of public headers.
- Use generic format IDs, variant IDs, capability queries, and operations.
- Detect an existing format from its contents, not its filename, unless the
  format requires a documented exception.
- Keep format code in the Writer backend. Do not add per-format GUI branches.
- Follow existing source layout and error-handling patterns.

The main public interface is `src/writer/api/peare/peare_writer.h`. Its
implementation is in `src/writer/api/Api.cpp`. Format backends live in
`src/writer/formats`.

## Operation model

| Requirement | Meaning |
|---|---|
| Create from zero | Create a new container or stream using an advertised format and variant. |
| Create file | Add a file from a source path to an existing target. |
| Create empty file | Add a zero-length file when the format supports entries. |
| Create directory | Add a directory when the format has a directory tree. |
| Replace file | Replace the contents of one existing file without changing its name. |
| Rename entry | Rename or move one file or directory. |
| Delete entry | Delete one file or directory. Support recursive deletion when required. |

Creation from zero uses the create-format and variant descriptors. The batch
`peare_writer_add_files_to_ex()` interface accepts the selected format, variant,
optional file system, and input-file list. Use one generic path for all formats.

For existing targets, use `peare_writer_get_update_capabilities()` to report
exactly which operations the selected target supports. Do not set a capability
because the operation is easy to implement for another format. Unsupported
operations must return `PEARE_WRITER_UNSUPPORTED_OPERATION` without corrupting
the target or ending an active transaction unexpectedly.

The ABI has separate calls for file creation, empty-file creation, directory
creation, replacement, rename, and deletion. Use entry-ID operations when names
are not unique. This applies to formats such as WAD, where repeated names can
identify distinct records.

## Implementation steps

1. Read `docs/ABI/API_WRITER.md`, the public Writer header, and the relevant
   backend and tests.
2. List every writable variant and every applicable operation before coding.
3. Define create-format and variant descriptors. Add filesystem choices when a
   new disk image needs a file system.
4. Implement create-from-zero for each advertised variant.
5. Implement existing-target discovery and truthful capability reporting.
6. Implement each applicable operation through the generic Writer ABI.
7. Stage changes before publication. Preserve the original target on failure.
8. Reopen and compare the result through the public Opener ABI.
9. Test create-from-zero through the GUI for every advertised variant and each
   advertised file-system choice. Run the sequence in one GUI process. Confirm
   each selection reaches the generic Writer ABI and the GUI Opener opens each
   result. Record process memory during the sequence to expose lifecycle leaks.
10. Add ABI tests for every variant and applicable operation. Update the format
    matrix and Writer ABI documentation.

Keep variant rules explicit. For example, compression methods, disk layouts,
file systems, and package payload codecs are separate variants when the ABI
exposes them as choices. An operation must not silently change an existing
target to another variant unless the format requires that change and the ABI
documents it.

## Transaction and format safety

- Never write to an original sample or disk image. Use a disposable copy.
- Stage the complete result before replacing the target.
- Validate the staged result before publication.
- Leave the original unchanged when parsing, an operation, or validation fails.
- Preserve untouched data and metadata where the format permits it.
- Reject malformed or unsupported structures instead of silently dropping data.
- Use checked arithmetic and bounds checks for all file offsets and sizes.
- Keep test output under `tmp`. Keep the total Peare test output below 10 GB.

For disk images, verify both the outer image and the inner file system. Reopen
the image and compare paths and payload hashes. Use an independent disk tool
when available.

## Verification

Use independently created samples or real samples with recorded origins. Read
`tests/POLICY.md` before adding or using a sample. Do not create format samples
from hand-authored bytes or format rules. Writer outputs can be test outputs,
but an independent implementation must verify them.

Test through the public C ABI. For each writable variant, cover:

- Creation from zero, including empty output when supported.
- Creation with one file and with multiple files when the ABI accepts a batch.
- Empty-file and directory creation when applicable.
- Replacement, rename, and deletion when applicable.
- Recursive directory deletion when the format has non-empty directories.
- Invalid names, duplicate names, missing entries, and failed transactions.
- Reopening the output through the Opener and checking names and payload hashes.
- GUI Tree and Details behavior through the generic Writer workflow.
- GUI create-from-zero for every advertised variant and file-system choice in
  one process, with memory samples across the sequence.
- Independent-tool validation of the output where a compatible tool exists.

Record pass, fail, and skip counts separately. A skipped test is not a pass.
Record the date, source commit, worktree state, variants, operations, interfaces,
and independent checks in the local Writer verification history under `tmp`.

## Completion checklist

- [ ] Every advertised writable variant can be created from zero.
- [ ] Every applicable CUD operation works through the public C ABI.
- [ ] Capabilities report the exact operations available for each target.
- [ ] Flat formats use `N/A` only for operations that have no meaning for them.
- [ ] The GUI uses the generic ABI and contains no format-specific branch.
- [ ] ABI tests cover all advertised variants and applicable operations.
- [ ] One persistent GUI test covers create-from-zero for every advertised
      variant and file-system choice and records process memory.
- [ ] GUI Tree and Details tests cover every applicable CUD operation for each
      advertised writable variant.
- [ ] An independent tool or decoder validates each output where available.
- [ ] Failure tests show that the original target remains unchanged.
- [ ] `SUPPORTED_FORMATS.md` says `Write: Yes` only for complete support.
- [ ] `docs/ABI/API_WRITER.md` describes the actual behavior and limits.
- [ ] The local verification history records the run and tested source revision.
