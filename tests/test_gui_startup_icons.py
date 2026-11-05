import os
import re
import subprocess
import sys
from pathlib import Path

import pytest


@pytest.mark.skipif(sys.platform != "win32", reason="Windows resource icons only")
def test_startup_icons_load_in_background(peare_gui_path, tmp_path):
    repo_root = peare_gui_path.resolve().parents[2]
    icon_source = Path(r"C:\Windows\System32\notepad.exe")
    if not icon_source.is_file():
        pytest.skip("Windows Notepad icon source is not available")
    settings_file = tmp_path / "startup-icons.ini"
    settings_file.write_text(
        "[RT_FOLDER_CLOSE]\n"
        f"path={icon_source.as_posix()}\n"
        "ordinal=#2\n"
        "iconExt=folder_close\n",
        encoding="utf-8")
    plugin_dir = Path(os.environ.get(
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        repo_root / "build-windows" / "test-plugins" / "platforms"))
    if not plugin_dir.is_dir():
        plugin_dir = peare_gui_path.resolve().parent / "platforms"

    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run(
        [str(peare_gui_path), "--startup-icons-test", str(settings_file),
         str(icon_source)],
        capture_output=True,
        text=True,
        env=environment,
        timeout=30,
        check=False)

    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"Loaded configured icons: (\d+)", result.stdout)
    assert match and int(match.group(1)) == 1, result.stdout + result.stderr
