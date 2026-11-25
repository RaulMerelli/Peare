"""Exercise the shipped MCP entry point and Peare's public ABIs."""

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "formats"


class Client:
    def __init__(self, process):
        self.process = process
        self.sequence = 0

    def request(self, method, params):
        self.sequence += 1
        request = {"jsonrpc": "2.0", "id": self.sequence, "method": method, "params": params}
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        body = json.loads(self.process.stdout.readline())
        assert body["id"] == self.sequence
        assert "error" not in body, body
        return body["result"]

    def call(self, tool, error=None, **arguments):
        result = self.request("tools/call", {"name": "peare_" + tool, "arguments": arguments})
        data = result["structuredContent"]
        if error:
            assert result["isError"], data
            assert data["code"] == error, data
        else:
            assert not result["isError"], data
        return data


@pytest.fixture
def mcp():
    binary = os.environ.get("PEARE_MCP_EXE")
    command = [binary] if binary else [sys.executable, str(ROOT / "tools" / "peare_mcp.py")]
    process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               text=True, encoding="utf-8")
    try:
        client = Client(process)
        result = client.request("initialize", {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "peare-test", "version": "1"},
        })
        assert result["serverInfo"]["name"] == "peare"
        process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        process.stdin.flush()
        yield client
    finally:
        process.stdin.close()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        process.stdout.close()
        assert process.returncode == 0


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def test_tool_discovery_and_nested_zip_browsing(mcp):
    tools = {tool["name"]: tool for tool in mcp.request("tools/list", {})["tools"]}
    assert not tools["peare_update"]["annotations"]["readOnlyHint"]
    assert tools["peare_browse"]["annotations"]["readOnlyHint"]
    sample = FIXTURES / "floppy/fixtures/FD14-Floppy.zip"
    root = mcp.call("browse", path=str(sample))
    assert root["format"] == "ZIP"
    assert [row["name"] for row in root["entries"]] == ["720k"]
    images = mcp.call("browse", ref=root["entries"][0]["ref"], limit=1)
    assert images["total"] == 3
    assert images["next_offset"] == 1
    assert images["entries"][0]["name"] == "x86BOOT.img"
    floppy = mcp.call("browse", ref=images["entries"][0]["ref"])
    assert any(row["name"] == "freedos" and row["kind"] == "folder" for row in floppy["entries"])
    data = mcp.call("read", path=str(sample), inside="720k/x86BOOT.img/freedos/version.fdi", **{"as": "hex"})
    assert data["sha256"] == "40066993c6bc408e12358913314a0cbfc1908ff13b3906ba0857e6b3d0b3958a"


@pytest.mark.parametrize(("fixture", "format_name"), [
    ("7z/fixtures/explicit-directories.7z", "SEVEN_ZIP"),
    ("rar4/fixtures/rar4-explicit-directories.rar", "RAR4"),
    ("rar5/fixtures/rar5-explicit-directories.rar", "RAR5"),
])
def test_archive_paths_and_extraction(mcp, tmp_path, fixture, format_name):
    path = str(FIXTURES / fixture)
    root = mcp.call("browse", path=path)
    assert root["format"] == format_name
    empty = next(row for row in root["entries"] if row["name"] == "empty")
    assert mcp.call("browse", ref=empty["ref"])["total"] == 0
    matches = mcp.call("find", path=path, name="settings.ini")
    assert matches["complete"]
    assert [row["matched_path"] for row in matches["matches"]] == ["nested/settings.ini"]
    output = tmp_path / "settings.ini"
    result = mcp.call("extract", ref=matches["matches"][0]["ref"], output=str(output))
    assert result["sha256"] == file_hash(output)
    assert result["sha256"] == "6a26f9b09f8614b865c066e504db2b104bca45e7bcc43f502dfabda1d0d84632"
    read = mcp.call("read", path=path, inside="nested/settings.ini", **{"as": "text", "limit": 30})
    assert read["text"].startswith("[RT_FOLDER_OPEN]")
    assert read["next_offset"] == 30


def test_explicit_split_wim_sources(mcp, tmp_path):
    fixtures = FIXTURES / "wim/fixtures"
    path = str(fixtures / "test.swm")
    parts = [path, str(fixtures / "test2.swm")]
    result = mcp.call("browse", path=path, parts=parts)
    assert result["format"] == "WIM"
    assert len(result["entries"]) == 4
    item = next(row for row in result["entries"] if row["name"] == "payload-4.bin")
    out = tmp_path / "four.bin"
    extracted = mcp.call("extract", ref=item["ref"], output=str(out))
    assert extracted["size"] == 409600
    assert extracted["sha256"] == file_hash(out)
    assert extracted["sha256"] == "a4ba7b8c701671de76fad7f13ca46560c1484e47c1c2e090b288d772e87848ac"
    assert mcp.call("capabilities", path=path)["update_targets"] == []


def test_writer_batches_on_a_copy_and_rejects_partial_updates(mcp, tmp_path):
    fixture = FIXTURES / "7z/fixtures/explicit-directories.7z"
    original = file_hash(fixture)
    new_archive = tmp_path / "edited.7z"
    ref = mcp.call("find", path=str(fixture), name="settings.ini")["matches"][0]["ref"]
    mcp.call("update", path=str(fixture), output=str(new_archive), changes=[
        {"action": "delete", "entry_ref": ref},
        {"action": "delete", "entry": "missing.txt"},
    ], error="writer_error")
    assert not new_archive.exists()
    result = mcp.call("update", path=str(fixture), output=str(new_archive), changes=[
        {"action": "delete", "entry_ref": ref},
        {"action": "add", "source": str(ROOT / "LICENSE"), "entry": "nested/added.txt"},
    ])
    assert result["verified"]["paths_checked"] == 2
    assert file_hash(fixture) == original
    assert mcp.call("find", path=str(new_archive), name="settings.ini")["matches"] == []
    for entry in ("root-readme.txt", "nested/deep/copyright.txt"):
        before = mcp.call("read", path=str(fixture), inside=entry, **{"as": "hex", "limit": 1})
        after = mcp.call("read", path=str(new_archive), inside=entry, **{"as": "hex", "limit": 1})
        assert before["sha256"] == after["sha256"]


def test_decode_and_convert_icon(mcp, tmp_path):
    source = ROOT / "resources/peare.ico"
    result = mcp.request("tools/call", {
        "name": "peare_read", "arguments": {"path": str(source), "as": "image"},
    })
    assert not result["isError"], result
    assert result["content"][1]["mimeType"] == "image/png"
    assert result["content"][1]["data"].startswith("iVBORw0KGgo")
    output = tmp_path / "converted"
    paths = mcp.call("convert", path=str(source), extension=".png", output=str(output))["converted"]
    assert all(Path(path).read_bytes().startswith(b"\x89PNG\r\n\x1a\n") for path in paths)


def test_preview_and_convert_group_icon(mcp, tmp_path):
    sample = FIXTURES / "pe/fixtures/MerelliBattleShip.exe"
    if not sample.is_file():
        pytest.skip("The PE sample is not present.")
    inside = ".rsrc/RT_GROUP_ICON/#32512/#0"
    result = mcp.request("tools/call", {
        "name": "peare_read", "arguments": {"path": str(sample), "inside": inside, "as": "image"},
    })
    assert not result["isError"], result
    assert result["structuredContent"]["images"] > 1
    converted = tmp_path / "group.ico"
    mcp.call("convert", path=str(sample), inside=inside, extension=".ico", output=str(converted))
    assert converted.read_bytes().startswith(b"\0\0\1\0")


def test_fonts_return_metadata_text_rendering_and_glyphs(mcp):
    sample = ROOT / "tests/abi_fonts/fixtures/4x6.ttf"
    info = mcp.call("read", path=str(sample), **{"as": "info"})
    assert info["fields"]["FONT_FACE_NAME"]
    chars = mcp.call("font", path=str(sample), mode="characters")
    assert chars["glyph_indices"]
    assert chars["total"] > 10
    for arguments in ({"text": "Peare MCP"}, {"glyph_index": 10}):
        render = mcp.request("tools/call", {
            "name": "peare_font", "arguments": {"path": str(sample), **arguments},
        })
        assert not render["isError"], render
        assert render["content"][1]["mimeType"] == "image/png"


def test_update_compressed_tar_by_reference(mcp, tmp_path):
    tar = shutil.which("tar")
    if not tar:
        pytest.skip("An independent tar utility is required.")
    original = tmp_path / "example.tar.gz"
    subprocess.run([tar, "-czf", str(original), "-C", str(ROOT), "README.md", "LICENSE"], check=True)
    original_hash = file_hash(original)
    root = mcp.call("browse", path=str(original))
    assert len(root["entries"]) == 1 and root["entries"][0]["kind"] == "container"
    archive = mcp.call("browse", ref=root["entries"][0]["ref"])
    entry = next(row for row in archive["entries"] if row["name"] == "README.md")
    result = mcp.call("update", path=str(original), changes=[{
        "action": "replace", "entry_ref": entry["ref"], "text": "Edited in Peare.",
    }])
    out = Path(result["updated"])
    assert file_hash(original) == original_hash
    assert subprocess.check_output([tar, "-xOzf", str(out), "README.md"]) == b"Edited in Peare."
    kept = subprocess.check_output([tar, "-xOzf", str(out), "LICENSE"])
    assert hashlib.sha256(kept).hexdigest() == file_hash(ROOT / "LICENSE")


def test_batch_uses_the_original_identity_of_duplicate_lumps(mcp, tmp_path):
    output = tmp_path / "duplicate.wad"
    mcp.call("create", output=str(output), format="wad", variant="pwad", inputs=[
        {"source": str(ROOT / "README.md"), "entry": "DUPE"},
        {"source": str(ROOT / "LICENSE"), "entry": "DUPE"},
        {"source": str(ROOT / "COPYRIGHT.md"), "entry": "THIRD"},
    ])
    matches = mcp.call("find", path=str(output), name="DUPE")["matches"]
    assert len(matches) == 2
    first = next(row for row in matches if row["size"] == (ROOT / "README.md").stat().st_size)
    second = next(row for row in matches if row["ref"] != first["ref"])
    read = mcp.call("update", path=str(output), changes=[
        {"action": "delete", "entry_ref": first["ref"]},
        {"action": "rename", "entry_ref": second["ref"], "new_entry": "RENAMED"},
    ])
    assert read["verified"]["lumps_checked"] == 2
    kept = mcp.call("read", path=read["updated"], inside="RENAMED", **{"as": "hex", "limit": 1})
    assert kept["sha256"] == file_hash(ROOT / "LICENSE")


@pytest.mark.parametrize("variant", ["monolithic-sparse", "two-gb-sparse"])
def test_update_vmdk_default_desktop_by_reference(mcp, tmp_path, variant):
    inputs = [{"source": str(ROOT / name), "entry": "Users/Default/Desktop/" + dest}
              for name, dest in (("README.md", "leggimi.txt"), ("LICENSE", "keep.txt"))]
    disk = Path(mcp.call("create", output=str(tmp_path / "source"),
                         container_name="guest.vmdk", format="vmdk", variant=variant,
                         filesystem="ntfs", inputs=inputs)["created"])
    original = {path: file_hash(path) for path in disk.parent.iterdir() if path.is_file()}
    root = mcp.call("browse", path=str(disk))
    partition = next(row for row in root["entries"] if row["type"] == "DISK_PARTITION")
    desktop = mcp.call("browse", ref=partition["ref"], inside="Users/Default/Desktop")
    selected = next(row for row in desktop["entries"] if row["name"] == "leggimi.txt")
    result = mcp.call("update", path=str(disk), changes=[{
        "action": "delete", "entry_ref": selected["ref"],
    }])
    assert result["target"]["disk_offset"] == partition["offset"]
    assert result["verified"]["paths_checked"] == 1
    assert all(file_hash(path) == digest for path, digest in original.items())
    updated = mcp.call("browse", path=result["updated"], inside=partition["path"] + "/Users/Default/Desktop")
    assert [row["name"] for row in updated["entries"]] == ["keep.txt"]
    kept = mcp.call("read", ref=updated["entries"][0]["ref"], **{"as": "hex", "limit": 1})
    assert kept["sha256"] == file_hash(ROOT / "LICENSE")
