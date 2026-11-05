# Agent Instructions

These rules apply to all work in this repository. Read the relevant documents
before changing code. Keep this file short and link to detailed documentation.

## Language and formatting

- Write all code, identifiers, comments, and commit messages in English.
- Keep code comments to three lines or fewer. Add comments only when they clarify non-obvious behavior.
- Follow the formatting used by the surrounding files. Do not reformat unrelated code.
- Use at most one consecutive blank line. Do not add trailing whitespace.
- Prefer ASCII in source files unless the file already requires another character set.
- Write technical documentation in clear, simple English.

## Architecture

- Keep the component boundaries clear: Opener opens and lists data, Decoder interprets payloads, and Writer creates or updates supported formats.
- Keep format-specific logic out of the GUI. The GUI uses generic operations through the public C ABIs.
- Keep public interfaces in C. Do not expose Qt, C++, STL, or exception types through an ABI.
- Do not make the Decoder or Writer depend on the Opener.
- Detect formats from content, not filename extensions, unless a format requires a documented exception.
- Each Opener module parses its own format. `OpenerSession` and `ModuleFactory` handle nested dispatch.
- Enumerate file-system directories on demand. Do not walk a full image tree during initial open.
- Preserve original payload bytes. Do not add synthetic roots or hide parser failures to make output look successful.

See [architecture](docs/architecture/ARCHITECTURE.md), [development](docs/development/DEVELOPMENT.md), and [module guidance](docs/development/ADDING_OPENER.md).

## Disk and file-system safety

- Never write to an original disk image or sample. Make and use a disposable copy for every write test.
- Keep each write transactional. Validate the staged result before publishing it, and leave the source unchanged on failure.
- Test writes by reopening the result and comparing existing paths and payload hashes. Use an external disk tool as an additional oracle when one is available.
- Use samples made by independent tools or real samples with documented rights. Never hand-author sample bytes or build sample files in project scripts.
- Record the source and creation method of every committed sample in its fixture README. Mark unknown origins as unknown.
- Keep independent-tool samples, real samples, and oracle results separate. State which evidence supports each compatibility claim.
- Do not claim production, forensic, or recovery reliability without specific evidence.
- Do not add copyrighted or otherwise restricted samples. Follow [fixture policy](tests/POLICY.md).

## Tests and builds

- Test through the public C ABI. Do not use private C++ internals as a substitute for ABI coverage.
- Use independent, license-safe samples and compare results with known ground truth.
- Run the narrow tests for the change, then the broader relevant suite when practical.
- On Windows, build with `tools/build/build_windows.bat`. Do not substitute another build command when the user asks to use the batch file.
- Report which tests, builds, samples, and external tools you actually ran. Separate verified results from assumptions.

See [test guidance](docs/testing/TESTING.md), [build instructions](docs/development/BUILDING.md), and the [format matrix](docs/SUPPORTED_FORMATS.md).

## Repository changes

- Read the relevant code and existing changes before editing. Preserve unrelated user changes.
- Keep changes focused. Do not revert or overwrite work that you did not make.
- Update the relevant documentation when behavior or support changes.
- Do not commit or push unless the user requests it.
