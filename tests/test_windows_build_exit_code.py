import os
import subprocess
from pathlib import Path

import pytest


@pytest.mark.skipif(os.name != "nt", reason="Windows batch semantics")
@pytest.mark.parametrize("build_status, expected", [(0, 0), (1, 1), (-1, 1)])
def test_windows_build_rejects_nonzero_status(tmp_path, build_status, expected):
    script = Path(__file__).resolve().parents[1] / "tools" / "build" / "build_windows.bat"
    lines = script.read_text(encoding="utf-8").splitlines()
    index = next(i for i, line in enumerate(lines) if line.startswith("cmake --build "))
    guard = lines[index + 1]
    assert guard.lower().endswith("goto :fail")
    probe = tmp_path / "build-status.bat"
    probe.write_text(
        "@echo off\n"
        f"cmd /c exit /b {build_status}\n"
        f"{guard}\n"
        "exit /b 0\n"
        ":fail\n"
        "exit /b 1\n",
        encoding="ascii",
    )
    result = subprocess.run([os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", str(probe)], check=False)
    assert result.returncode == expected
