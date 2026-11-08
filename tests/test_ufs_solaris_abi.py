import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import ROOT
from peare_opener import CONTAINER_NAMES, _Context, _HANDLE
from run_gui_preview_tests import valid_png


SAMPLE = Path(os.environ.get(
    "PEARE_UFS_SOLARIS_SAMPLE",
    Path.home() / "Documents" / "inerente a" / "Peare" /
    "Solaris-UFS-sample" / "sparc.qcow2"))
RAW = Path(os.environ.get(
    "PEARE_UFS_SOLARIS_RAW", ROOT / "tests" / "output" /
    "solaris-ufs-working.raw"))
TSK_BIN = Path(os.environ.get(
    "PEARE_TSK_BIN",
    Path.home() / "Documents" / "inerente a" / "Peare" /
    "Research tools" / "SleuthKit-4.15.0" / "unpacked" /
    "sleuthkit-4.15.0-win32" / "bin"))
TSK_SUFFIX = ".exe" if os.name == "nt" else ""
INDIRECT_FILE = "usr/lib/lp/postscript/postprint.ps.iso8859-2"
DOUBLE_INDIRECT_FILE = "usr/openwin/lib/llib-lX11.ln"
GUI_FILE = "etc/devlink.tab"
GUI_TREE_PATH = "Whole disk/" + GUI_FILE


def _require_samples():
    tools = [TSK_BIN / (name + TSK_SUFFIX) for name in ("fls", "icat", "fsstat")]
    if not SAMPLE.is_file() or not RAW.is_file() or not all(path.is_file() for path in tools):
        pytest.skip("Solaris UFS sample, working raw image, or Sleuth Kit is unavailable")


def _tsk(*args):
    executable = TSK_BIN / (args[0] + TSK_SUFFIX)
    result = subprocess.run([str(executable), *args[1:]], capture_output=True,
                            check=False)
    if result.returncode:
        raise AssertionError(result.stderr.decode("utf-8", "replace"))
    return result.stdout


def _entries(opener):
    entries = []
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, resource) == 0
            try:
                context = _Context()
                assert opener._L.peare_resource_get_context(
                    resource, context) == 0
                entries.append((
                    context.identifier_utf8.to_bytes().decode("utf-8", "replace"),
                    bool(context.is_container),
                    CONTAINER_NAMES.get(context.container_format,
                                        str(context.container_format)),
                ))
                opener._L.peare_resource_context_free(context)
            finally:
                opener._L.peare_resource_destroy(resource)
    return entries


def _open_container(library, opener, name, expected_format=None):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, resource) == 0
            try:
                context = _Context()
                assert opener._L.peare_resource_get_context(
                    resource, context) == 0
                identifier = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                is_container = bool(context.is_container)
                format_name = CONTAINER_NAMES.get(
                    context.container_format, str(context.container_format))
                opener._L.peare_resource_context_free(context)
                if identifier != name:
                    continue
                assert is_container, f"{name} is not a container"
                if expected_format:
                    assert format_name == expected_format
                source = _HANDLE()
                assert opener._L.peare_resource_get_source(resource, source) == 0
                try:
                    child = library._open_source(source)
                    assert child is not None
                    return child
                finally:
                    opener._L.peare_source_destroy(source)
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"container not found: {name}")


def _open_path(library, opener, path):
    parts = path.split("/")
    current = opener
    owned = []
    try:
        for part in parts[:-1]:
            child = _open_container(library, current, part)
            owned.append(child)
            current = child
        for folder in range(current.folder_count()):
            for index in range(current.resource_count(folder)):
                resource = _HANDLE()
                assert current._L.peare_opener_open_resource_at(
                    current._h, folder, index, resource) == 0
                context = _Context()
                try:
                    assert current._L.peare_resource_get_context(
                        resource, context) == 0
                    name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                    size = int(context.data_size)
                    if name == parts[-1]:
                        data = bytearray()
                        offset = 0
                        while offset < size:
                            chunk = library.read_resource(
                                resource, offset, min(1024 * 1024, size - offset))
                            assert chunk
                            data.extend(chunk)
                            offset += len(chunk)
                        return bytes(data)
                finally:
                    current._L.peare_resource_context_free(context)
                    current._L.peare_resource_destroy(resource)
        raise AssertionError(f"file not found: {path}")
    finally:
        for child in reversed(owned):
            child.close()


def _tsk_inode(path):
    listing = _tsk("fls", "-r", "-p", "-f", "ufs", str(RAW)).decode(
        "utf-8", "replace")
    target = path.lstrip("/")
    for line in listing.splitlines():
        if ":\t" not in line:
            continue
        metadata, name = line.split(":\t", 1)
        if name == target:
            match = re.search(r"(\d+)\s*$", metadata)
            if match:
                return match.group(1)
    raise AssertionError(f"Sleuth Kit did not list {path}")


def test_solaris_ufs1_direct_and_qcow_tree_match_sleuthkit(peare_lib):
    _require_samples()
    assert b"File System Type: UFS 1" in _tsk("fsstat", "-f", "ufs", str(RAW))
    expected = set()
    root_listing = _tsk("fls", "-f", "ufs", str(RAW)).decode("utf-8", "replace")
    for line in root_listing.splitlines():
        match = re.match(r"^\S+\s+\d+:\t(.+)$", line)
        if match and match.group(1) != "$OrphanFiles":
            expected.add(match.group(1))
    assert expected

    with peare_lib.open_file(str(RAW)) as direct:
        assert direct.container_format() == "UFS"
        assert {name for name, _, _ in _entries(direct)} == expected

    with peare_lib.open_file(str(SAMPLE)) as image:
        assert image.container_format() == "QCOW2"
        ufs = _open_container(peare_lib, image, "Whole disk", "UFS")
        try:
            assert {name for name, _, _ in _entries(ufs)} == expected
        finally:
            ufs.close()


def test_solaris_ufs1_indirect_file_matches_sleuthkit(peare_lib):
    _require_samples()
    inode = _tsk_inode(INDIRECT_FILE)
    expected = _tsk("icat", "-f", "ufs", str(RAW), inode)
    assert len(expected) > 12 * 8192
    with peare_lib.open_file(str(SAMPLE)) as image:
        actual = _open_path(peare_lib, image, "Whole disk/" + INDIRECT_FILE)
    assert hashlib.sha256(actual).digest() == hashlib.sha256(expected).digest()


def test_solaris_ufs1_double_indirect_file_matches_sleuthkit(peare_lib):
    _require_samples()
    inode = _tsk_inode(DOUBLE_INDIRECT_FILE)
    expected = _tsk("icat", "-f", "ufs", str(RAW), inode)
    assert len(expected) > 12 * 8192 + (8192 // 4) * 8192
    with peare_lib.open_file(str(SAMPLE)) as image:
        actual = _open_path(peare_lib, image, "Whole disk/" + DOUBLE_INDIRECT_FILE)
    assert hashlib.sha256(actual).digest() == hashlib.sha256(expected).digest()


def test_solaris_ufs1_gui_offscreen_preview_matches_sleuthkit(peare_gui_path):
    _require_samples()
    inode = _tsk_inode(GUI_FILE)
    expected = _tsk("icat", "-f", "ufs", str(RAW), inode)
    output_dir = ROOT / "tests" / "output" / "ufs-solaris"
    output_dir.mkdir(parents=True, exist_ok=True)
    screenshot = output_dir / "devlink-tab.png"
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    plugin_name = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    candidates = [
        ROOT / "build-windows" / "test-plugins" / "platforms",
        peare_gui_path.parent / "platforms",
    ]
    plugin_override = environment.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    plugin_dir = Path(plugin_override) if plugin_override else None
    if plugin_dir is None or not (plugin_dir / plugin_name).is_file():
        plugin_dir = next((path for path in candidates
                           if (path / plugin_name).is_file()), candidates[-1])
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run([
        str(peare_gui_path), "--preview-resource", str(SAMPLE),
        GUI_TREE_PATH, str(screenshot),
    ], capture_output=True, text=True, env=environment, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"]
    assert inspection["size"] == len(expected)
    assert inspection["sha256"] == hashlib.sha256(expected).hexdigest()
    assert not inspection.get("duplicate_paths")
    assert valid_png(screenshot)
