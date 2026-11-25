import json
import os
import subprocess
import sys
from pathlib import Path

import pytest


def test_encrypted_dedup_vault_password_dialog(peare_gui_path):
    root = os.environ.get("PEARE_ACRONIS_SAMPLES")
    if not root:
        pytest.skip("Set PEARE_ACRONIS_SAMPLES to the independent sample directory")
    archive = (Path(root) / "acronis15-created" / "dedup-encrypted-20261116" /
               "Vault" / "encrypted-sample.tib")
    if not archive.is_file():
        pytest.skip("Acronis encrypted deduplicated sample not available")
    project = Path(__file__).resolve().parents[2]
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment.pop("PEARE_PREVIEW_PASSWORD", None)
    plugin = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    configured = environment.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    candidates = ([Path(configured)] if configured else []) + [
        project / "build-windows" / "test-plugins" / "platforms",
        peare_gui_path.parent / "platforms",
    ]
    plugin_dir = next((p for p in candidates if (p / plugin).is_file()), None)
    assert plugin_dir is not None, "Offscreen Qt plugin is missing"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    options = {
        "archive": str(archive),
        "key_file": str(archive),
        "password": "PeareDedupTest2026!",
        "resource_path": "C:/PeareDedup/Input/readme.txt",
        "expected_sha256": "2ae23fc848095781970d15b5abae83b57f1b5cff576bc891e3c1e423cbb1013c",
    }
    result = subprocess.run(
        [str(peare_gui_path), "--gui-credential-test", "password-only"],
        input=json.dumps(options), text=True, capture_output=True, env=environment,
        timeout=90, check=False,
    )
    output = (result.stdout + result.stderr).replace(options["password"], "[redacted]")
    assert result.returncode == 0, output
    assert "GUI credential scenario passed: password-only" in result.stdout
