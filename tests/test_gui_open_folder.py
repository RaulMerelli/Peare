import os
import subprocess
import sys
from pathlib import Path


def test_open_supported_archive_from_folder_details(peare_gui_path, tmp_path):
    root = Path(__file__).resolve().parents[1]
    sample_folder = root / "tests" / "formats" / "tar" / "fixtures"
    sample = sample_folder / "docs.tar"
    assert sample.is_file()

    plugin_name = "qoffscreen.dll" if sys.platform == "win32" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    configured = os.environ.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    candidates = [root / "build-windows" / "test-plugins" / "platforms",
                  root / "build" / "test-plugins" / "platforms",
                  peare_gui_path.resolve().parent / "platforms"]
    plugin_dir = Path(configured) if configured else next(
        (path for path in candidates if (path / plugin_name).is_file()), candidates[-1])
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    screenshot = tmp_path / "open-folder-details.png"
    result = subprocess.run(
        [str(peare_gui_path), "--folder-open-test", str(sample_folder),
         sample.name, str(screenshot)],
        capture_output=True, text=True, env=environment, timeout=30, check=False)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Folder file opened through Peare." in result.stdout
    assert screenshot.is_file() and screenshot.stat().st_size > 1024
