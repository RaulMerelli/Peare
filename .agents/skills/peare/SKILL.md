---
name: peare
description: Use Peare to inspect, extract, decode, create, or update supported files through its public APIs.
---

# Use Peare

Use this skill when a task needs inspection or transformation of a file with
Peare. Do not use Peare as a substitute for a forensic or recovery tool.

## Start

Run commands from the Peare repository root. Build Peare first when the shared
libraries are missing. On Windows, run `tools/build/build_windows.bat`.

```powershell
python tools/peare_agent.py inspect "C:\data\input.img" --max-depth 8
python tools/peare_agent.py inspect "C:\data\input.zip" --hash-limit 67108864
```

The command returns JSON. It lists the detected format and resources. Nested
containers appear below their parent resource. Hashing is optional and only
reads leaf payloads up to the selected limit.

## Extract And Decode

Use the resource path from `inspect` to extract one payload. The command refuses
to replace an existing output unless `--force` is present.

```powershell
python tools/peare_agent.py extract "C:\data\input.zip" "folder/file.bin" "C:\work\file.bin"
```

Use the Decoder for payloads that need image or text conversion. Pass platform
and resource type when the Opener provides them.

```powershell
python tools/peare_agent.py decode "C:\work\bitmap.bin" --kind images --output-dir "C:\work\decoded" --platform Windows --type RT_BITMAP
python tools/peare_agent.py decode "C:\work\strings.bin" --kind texts --output-dir "C:\work\decoded"
python tools/peare_agent.py decode "C:\work\bitmap.bin" --kind info --platform Windows --type RT_BITMAP
```

The Decoder writes PNG or UTF-8 text files, or returns available metadata as
JSON. It does not change the input.

## Create And Update

Query the Writer before you create or update a container. Use the exact format,
variant, filesystem, and target IDs from its JSON output.

```powershell
python tools/peare_agent.py writer formats
python tools/peare_agent.py writer targets "C:\work\copy.vhd"
python tools/peare_agent.py writer create "C:\work\new.zip" --format zip --variant store --input "C:\work\file.txt=folder/file.txt"
```

The target query lists supported operations for each target. Update only a
disposable copy. The `mutate` command requires `--confirm-copy` before it writes.

```powershell
python tools/peare_agent.py writer mutate "C:\work\copy.zip" --confirm-copy add --input "C:\work\file.txt=folder/file.txt"
python tools/peare_agent.py writer mutate "C:\work\copy.zip" --confirm-copy rename --entry "folder/old.txt" --new-name "folder/new.txt"
python tools/peare_agent.py writer mutate "C:\work\copy.zip" --confirm-copy delete --entry "folder/new.txt"
```

After each update, inspect the output again. Compare existing paths and payload
hashes with the source copy. Do not claim success when the Writer returns an
error or when the reopened output differs unexpectedly.

## API References

- [Supported format matrix](../../../docs/SUPPORTED_FORMATS.md)
- [Opener API](../../../docs/ABI/API_OPENER.md)
- [Decoder API](../../../docs/ABI/API_DECODER.md)
- [Writer API](../../../docs/ABI/API_WRITER.md)
- [CLI source](../../../tools/peare_agent.py)
