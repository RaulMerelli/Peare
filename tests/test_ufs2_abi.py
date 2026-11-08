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


SAMPLE = ROOT / "tests" / "fixtures" / "ufs2" / "ufs2.raw"
TSK_BIN = Path(os.environ.get(
    "PEARE_TSK_BIN",
    Path.home() / "Documents" / "inerente a" / "Peare" /
    "Research tools" / "SleuthKit-4.15.0" / "unpacked" /
    "sleuthkit-4.15.0-win32" / "bin"))
TSK_SUFFIX = ".exe" if os.name == "nt" else ""
PARTITION_PATH = "Partition 1 — BSD disklabel partition"
SAMPLE_SHA256 = "ee5d99e5e60aa566b9e4dd0a5341c1e2b433cbbfcebffbdbf0ad2a3e2153cd67"
PASSWORDS_SHA256 = "02a2a6af2f1ecf4720d7d49d640f0d0a269a7ec733e41973bdd34f09dad0e252"


def _tsk(*args):
    executable = TSK_BIN / (args[0] + TSK_SUFFIX)
    result = subprocess.run([str(executable), *args[1:]], capture_output=True,
                            check=False)
    if result.returncode:
        raise AssertionError(result.stderr.decode("utf-8", "replace"))
    return result.stdout


def _open_partition(library, disk):
    for folder in range(disk.folder_count()):
        for index in range(disk.resource_count(folder)):
            resource = _HANDLE()
            assert disk._L.peare_opener_open_resource_at(
                disk._h, folder, index, resource) == 0
            try:
                context = _Context()
                assert disk._L.peare_resource_get_context(resource, context) == 0
                name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                is_container = bool(context.is_container)
                fmt = CONTAINER_NAMES.get(context.container_format, "")
                disk._L.peare_resource_context_free(context)
                if not is_container or fmt != "UFS":
                    continue
                source = _HANDLE()
                assert disk._L.peare_resource_get_source(resource, source) == 0
                try:
                    child = library._open_source(source)
                    assert child is not None
                    return name, child
                finally:
                    disk._L.peare_source_destroy(source)
            finally:
                disk._L.peare_resource_destroy(resource)
    raise AssertionError("BSD disklabel UFS2 partition was not exposed")


def _root_entries(opener):
    entries = []
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, resource) == 0
            try:
                context = _Context()
                assert opener._L.peare_resource_get_context(resource, context) == 0
                entries.append((
                    context.identifier_utf8.to_bytes().decode("utf-8", "replace"),
                    bool(context.is_container),
                ))
                opener._L.peare_resource_context_free(context)
            finally:
                opener._L.peare_resource_destroy(resource)
    return entries


def _read_file(opener, name):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, resource) == 0
            try:
                context = _Context()
                assert opener._L.peare_resource_get_context(resource, context) == 0
                identifier = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                size = int(context.data_size)
                opener._L.peare_resource_context_free(context)
                if identifier != name:
                    continue
                data = bytearray()
                offset = 0
                while offset < size:
                    chunk = opener._lib.read_resource(
                        resource, offset, min(1024 * 1024, size - offset))
                    assert chunk
                    data.extend(chunk)
                    offset += len(chunk)
                return bytes(data)
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"UFS2 file not found: {name}")


def _require_samples():
    if not SAMPLE.is_file():
        pytest.skip("UFS2 fixture is unavailable")
    assert hashlib.sha256(SAMPLE.read_bytes()).hexdigest() == SAMPLE_SHA256


def _has_tsk():
    return all((TSK_BIN / (name + TSK_SUFFIX)).is_file()
               for name in ("fls", "fsstat", "icat"))


def test_bsd_disklabel_ufs2_abi_matches_recorded_oracle(peare_lib):
    _require_samples()
    expected_root = {".snap", "a_directory", "passwords.txt", "a_link"}
    if _has_tsk():
        assert b"File System Type: UFS 2" in _tsk(
            "fsstat", "-o", "16", "-f", "ufs2", str(SAMPLE))
        oracle_root = set()
        root_listing = _tsk("fls", "-o", "16", "-f", "ufs2", str(SAMPLE))
        for line in root_listing.decode("utf-8", "replace").splitlines():
            match = re.match(r"^\S+\s+\d+:\t(.+)$", line)
            if match and match.group(1) != "$OrphanFiles":
                oracle_root.add(match.group(1))
        assert oracle_root == expected_root

    with peare_lib.open_file(str(SAMPLE)) as disk:
        assert disk.container_format() == "RAW_DISK"
        partition_name, ufs = _open_partition(peare_lib, disk)
        try:
            assert partition_name == PARTITION_PATH
            assert ufs.container_format() == "UFS"
            assert {name for name, _ in _root_entries(ufs)} == expected_root
            data = _read_file(ufs, "passwords.txt")
            assert hashlib.sha256(data).hexdigest() == PASSWORDS_SHA256
            assert len(data) == 116
        finally:
            ufs.close()


def test_ufs2_gui_preview_payload_matches_oracle(peare_gui_path):
    _require_samples()
    if _has_tsk():
        expected = _tsk("icat", "-o", "16", "-f", "ufs2", str(SAMPLE), "4")
        assert hashlib.sha256(expected).hexdigest() == PASSWORDS_SHA256
    output = ROOT / "tests" / "output" / "ufs2" / "passwords.png"
    output.parent.mkdir(parents=True, exist_ok=True)
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
        PARTITION_PATH + "/passwords.txt", str(output),
    ], capture_output=True, text=True, env=environment, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"]
    assert inspection["size"] == 116
    assert inspection["sha256"] == PASSWORDS_SHA256
    assert not inspection.get("duplicate_paths")
    assert valid_png(output)
