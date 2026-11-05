# AI agent use

The repository includes a JSON CLI for agents and scripts that use Peare. The
CLI calls the public C ABIs for PeareOpener, PeareDecoder, and PeareWriter.
Use `decode --kind info` to query available payload metadata.

Run it from the repository root:

```powershell
python tools/peare_agent.py --help
```

Build the shared libraries first when they are not present. On Windows, use
[`build_windows.bat`](../../tools/build/build_windows.bat). The CLI searches the standard
build folders. Set `PEARE_OPENER_LIB`, `PEARE_DECODER_LIB`, or
`PEARE_WRITER_LIB` to select a different library.

The [Peare agent skill](../../.agents/skills/peare/SKILL.md) describes the
inspection, extraction, decoding, and update workflows. The skill requires a
disposable copy before each Writer update.
