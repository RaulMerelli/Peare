"""Check explicit archive directories and nested members through the Opener ABI."""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def _tool(env_name, candidates):
    configured = os.environ.get(env_name)
    paths = [configured, *(shutil.which(name) for name in candidates)]
    program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
    paths.append(str(program_files / "7-Zip" / "7z.exe") if "7ZIP" in env_name else
                 str(program_files / "WinRAR" / "rar.exe"))
    return next((path for path in paths if path and Path(path).is_file()), None)


def _make_tree(root, filename):
    deep = root / "nested" / "deep"
    deep.mkdir(parents=True)
    (root / "empty").mkdir()
    shutil.copyfile(ROOT / filename, deep / filename)


def _assert_tree(peare_lib, archive, expected_leaf, expected_source):
    opener = peare_lib.open_file(str(archive))
    assert opener is not None, archive
    with opener:
        rows = opener.normalized_tree()

    expected_paths = {"empty", "nested", "nested/deep", expected_leaf}
    actual_paths = {row["path"] for row in rows}
    assert actual_paths == expected_paths
    directories = {row["path"] for row in rows if row["path"] != expected_leaf}
    assert all(not row["is_container"] for row in rows if row["path"] in directories)
    leaf = next(row for row in rows if row["path"] == expected_leaf)
    assert (expected_source.stat().st_size, hashlib.sha256(expected_source.read_bytes()).hexdigest()) == (
        leaf["size"], leaf["sha256"])


def test_7z_explicit_directories_keep_their_children(peare_lib, tmp_path):
    seven_zip = _tool("PEARE_7ZIP_EXE", ("7z", "7z.exe", "7za.exe"))
    if not seven_zip:
        pytest.skip("7-Zip is required to create the independent archive sample")

    source = tmp_path / "7z-source"
    _make_tree(source, "README.md")
    archive = tmp_path / "directory-tree.7z"
    subprocess.run(
        [seven_zip, "a", "-t7z", str(archive), str(source / "*")],
        check=True, capture_output=True,
    )
    subprocess.run([seven_zip, "t", str(archive)], check=True, capture_output=True)
    _assert_tree(peare_lib, archive, "nested/deep/README.md", source / "nested" / "deep" / "README.md")


@pytest.mark.parametrize(("format_name", "rar_switch"), (("rar4", "-ma4"), ("rar5", "-ma5")))
def test_rar_directory_entries_are_not_ghost_paths(peare_lib, tmp_path, format_name, rar_switch):
    rar = os.environ.get("PEARE_RAR_EXE") or str(
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "WinRAR" / "rar.exe")
    if not Path(rar).is_file():
        pytest.skip("WinRAR is required to create and verify the RAR sample")

    source = tmp_path / f"{format_name}-source"
    _make_tree(source, "AGENTS.md")
    archive = tmp_path / f"directory-tree-{format_name}.rar"
    subprocess.run(
        [rar, "a", rar_switch, "-ep1", "-r", str(archive), str(source / "*")],
        check=True, capture_output=True,
    )
    subprocess.run([rar, "t", str(archive)], check=True, capture_output=True)
    subprocess.run([rar, "lt", str(archive)], check=True, capture_output=True)
    _assert_tree(
        peare_lib, archive, "nested/deep/AGENTS.md",
        source / "nested" / "deep" / "AGENTS.md",
    )
