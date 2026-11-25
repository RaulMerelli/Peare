# Peare in an AI chat

Peare MCP connects a local AI client to Peare's Opener, Decoder, and Writer.
It works with local files and starts when the client needs it.

## Install

Download a package from the [Peare CI release](https://github.com/RaulMerelli/Peare/releases/tag/ci-latest).
Each package includes a runtime. You need the AI client on the same machine as
the files.

### Claude Desktop on Windows

1. Download [peare-mcp-windows-x64.mcpb](https://github.com/RaulMerelli/Peare/releases/download/ci-latest/peare-mcp-windows-x64.mcpb).
2. Open the file in Claude Desktop.
3. Select **Install**.
4. Open a new chat.

See [Claude Desktop extensions](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop).

### Codex and Claude Code on Windows

1. Download [peare-mcp-windows-x64.zip](https://github.com/RaulMerelli/Peare/releases/download/ci-latest/peare-mcp-windows-x64.zip).
2. Extract it to a permanent folder, for example `C:\Tools\PeareMcp`.
3. Run the command for your client in PowerShell.

```powershell
codex mcp add peare -- 'C:\Tools\PeareMcp\PeareMcp.exe'
claude mcp add --scope user --transport stdio peare -- 'C:\Tools\PeareMcp\PeareMcp.exe'
```

4. Restart the client and open a new chat.

If you use the Codex app without its CLI, add a local STDIO server in the MCP
settings. Set its command to the absolute path of `PeareMcp.exe`.

### Codex and Claude Code on Linux

1. Download [peare-mcp-linux-x64.tar.gz](https://github.com/RaulMerelli/Peare/releases/download/ci-latest/peare-mcp-linux-x64.tar.gz).
2. Extract it to a permanent folder, for example `$HOME/.local/opt/peare-mcp`.
3. Run the command for your client.

```sh
codex mcp add peare -- "$HOME/.local/opt/peare-mcp/peare-mcp"
claude mcp add --scope user --transport stdio peare -- "$HOME/.local/opt/peare-mcp/peare-mcp"
```

4. Restart the client and open a new chat.

The Linux package targets Ubuntu 22.04 or later with a standard desktop
runtime. See the [Codex MCP guide](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)
and the [Claude Code MCP guide](https://code.claude.com/docs/en/mcp).

## Ask for a file operation

Give the client a local path or identify a file in its current workspace.
You can give it requests such as these.

- “Look inside `sources.7z`. Tell me what is in `docs`.”
- “Find and read `README.txt` inside `install.swm`.”
- “Show the main icon inside `tool.exe` and export it as an ICO file.”
- “Delete `leggimi.txt` from the default Windows user's Desktop in
  `C:\VMs\guest.vmdk`. Give me the updated disk.”

For a disk change, shut down the guest first. If Windows has several plausible
profiles, tell the client which profile you mean. If a large disk exceeds the
Codex tool timeout, set `tool_timeout_sec = 1800` under `[mcp_servers.peare]`
in the Codex configuration and restart the client.

For the VMDK request, the client can browse the partitions, then open
`Users/Default/Desktop` in the Windows partition. The file's reference identifies
the partition and the path for the update. The result includes the path to
the copy and the checks that Peare made after it reopened the copy.

## What Peare can do

| Operation | Behavior |
| --- | --- |
| Browse and find | Lists one folder at a time. Follows partitions, lazy directories, and nested containers. Finds an entry by name or glob. |
| Read and decode | Returns text, image previews, metadata, conversions, or hex. Uses the resource context when it is available. |
| Export and convert | Saves the source bytes, a directory tree, or a converted resource. |
| Inspect fonts | Lists collection faces and character mappings. Renders text or one glyph. |
| Create | Queries the Writer for format, variant, and filesystem choices. |
| Update | Queries the Writer for targets and operations. Applies changes in one transaction, reopens the result, and publishes a new copy. |

Peare detects the format from the file contents where the format permits it.
The [format matrix](../SUPPORTED_FORMATS.md) lists supported variants. The
Writer reports what it can do with the specific file.

The server returns a new path for every update. For a single file, the default
name adds `.edited` before its extension. For a set with companion files, the
default is a new directory with `.edited` after the original name. Use the
returned path to make additional changes. Keep the files in a set together.

The server can open SWM sets, but the Writer cannot update them. It can browse
embedded containers, but it cannot write through an arbitrary chain of
containers. It can update supported compressed TAR and Debian data archives
through their outer file. For other embedded containers, export the container
and update it as a separate file.
Credentials that you pass to a tool stay in the server process for subsequent
calls. Your AI client controls tool access and can record tool inputs.

## Run from source

Build the libraries with the [platform build instructions](../development/BUILDING.md).
Register Python and the absolute path to `tools/peare_mcp.py` as the STDIO
command and its first argument. The [Peare skill](../../.agents/skills/peare/SKILL.md)
also documents the JSON CLI for environments without the MCP server.
