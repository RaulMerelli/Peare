import base64
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest


@pytest.mark.parametrize("mode", [
    "key", "password", "cancel-retry", "wrong-key-retry", "key-to-password", "password-to-key",
    "key-only", "password-only", "encrypted-key", "encrypted-key-retry", "encrypted-key-kdf-limit",
])
def test_tibx_credential_dialogs(peare_gui_path, tmp_path, mode):
    sample_root = os.environ.get("PEARE_ACRONIS_SAMPLES")
    if not sample_root:
        pytest.skip("Set PEARE_ACRONIS_SAMPLES to the independent sample directory")
    sample = Path(sample_root) / "public-key-aes128-20261115"
    encrypted_key = mode in ("encrypted-key", "encrypted-key-retry", "encrypted-key-kdf-limit")
    archive = (sample / "rsa-aes128-certificate-only.tibx" if mode == "key-only" or encrypted_key else
               Path(sample_root) / "acronis15-created" / "modern-aes128.tibx" if mode == "password-only" else
               sample / "rsa-aes128.tibx")
    key = sample / "test-private.pem"
    if not archive.is_file() or not key.is_file():
        pytest.skip("The independent Acronis RSA sample and test key are not available")
    expected_hash = ("6a69ce2864d2dda3acf506ee4c08e28cee5259882053a86aecafc5fdc75c2b88"
                     if mode == "key-only" or encrypted_key else
                     "c92ca50ca09f474e6f782747823fd86f071d1faaa639f64ea2929ade5831dd8d"
                     if mode == "password-only" else
                     "ffdf0e557709fef5636a72492b69936dfdc78e84024faa38449300d7235da55e")
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == expected_hash

    wrong_key = tmp_path / "invalid-key.pem"
    wrong_key.write_bytes(b"not a private key\n")
    passphrase = "chiave-\u00e8"
    if encrypted_key:
        serialization = pytest.importorskip("cryptography.hazmat.primitives.serialization")
        private_key = serialization.load_pem_private_key(key.read_bytes(), password=None)
        encrypted_path = tmp_path / "encrypted-key.pem"
        encoded_key = private_key.private_bytes(
            serialization.Encoding.DER if mode == "encrypted-key-kdf-limit" else serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.BestAvailableEncryption(passphrase.encode("utf-8")),
        )
        if mode == "encrypted-key-kdf-limit":
            decoder = pytest.importorskip("pyasn1.codec.der.decoder")
            encoder = pytest.importorskip("pyasn1.codec.der.encoder")
            envelope, remainder = decoder.decode(encoded_key)
            assert not remainder
            envelope[0][1][0][1][1] = 1000001
            encoded_key = (b"-----BEGIN ENCRYPTED PRIVATE KEY-----\n" +
                           base64.b64encode(encoder.encode(envelope)) +
                           b"\n-----END ENCRYPTED PRIVATE KEY-----\n")
        encrypted_path.write_bytes(encoded_key)
        key = encrypted_path
    options = {
        "archive": str(archive),
        "key_file": str(key),
        "wrong_key_file": str(wrong_key),
        "password": "PeareTest2026",
        "passphrase": passphrase,
    }
    root = Path(__file__).resolve().parents[2]
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment.pop("PEARE_PREVIEW_PASSWORD", None)
    plugin = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    configured = environment.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    candidates = ([Path(configured)] if configured else []) + [
        root / "build-windows" / "test-plugins" / "platforms",
        peare_gui_path.parent / "platforms",
    ]
    plugin_dir = next((p for p in candidates if (p / plugin).is_file()), None)
    assert plugin_dir is not None, "Offscreen Qt plugin is missing"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run(
        [str(peare_gui_path), "--gui-credential-test", mode],
        input=json.dumps(options), text=True, capture_output=True, env=environment,
        timeout=90, check=False,
    )
    output = result.stdout + result.stderr
    for secret in (str(key), str(wrong_key), options["password"], passphrase):
        output = output.replace(secret, "[redacted]")
    assert result.returncode == 0, output
    assert "GUI credential scenario passed: " + mode in result.stdout
