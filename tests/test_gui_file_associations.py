import os
import re
import subprocess
from pathlib import Path

def test_file_association_settings_dialog(peare_gui_path):
    root = Path(__file__).resolve().parents[1]
    output = root / "tests" / "output"
    output.mkdir(parents=True, exist_ok=True)
    screenshot = output / "file-associations.png"
    plugin_dir = os.environ.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    if not plugin_dir:
        candidates = [root / "build-windows" / "test-plugins" / "platforms",
                      root / "build" / "test-plugins" / "platforms",
                      peare_gui_path.resolve().parent / "platforms"]
        plugin_names = ("qoffscreen.dll", "libqoffscreen.so", "libqoffscreen.dylib")
        plugin_dir = str(next((path for path in candidates
                               if any((path / name).is_file() for name in plugin_names)),
                              candidates[-1]))

    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run(
        [str(peare_gui_path), "--file-associations-test", str(screenshot)],
        capture_output=True,
        text=True,
        env=environment,
        timeout=30,
        check=False)

    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"(\d+) categories, (\d+) extension choices", result.stdout)
    assert match and int(match.group(1)) == 10 and int(match.group(2)) >= 200
    assert screenshot.is_file() and screenshot.stat().st_size > 1024
