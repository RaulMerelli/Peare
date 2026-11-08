import json
import hashlib
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

from conftest import ROOT
from peare_opener import _Context, _HANDLE
from run_gui_preview_tests import valid_png


FIXTURES = ROOT / "tests" / "fixtures" / "xnb-monogame"
LZ4_FIXTURE = ROOT / "tests" / "fixtures" / "xnb-lz4" / "white-1.xnb"
V4_FIXTURE = ROOT / "tests" / "fixtures" / "xnb-v4" / "LogoOnly_64px-v4.xnb"
CASES = {
    "BlenderDefaultCube.xnb": ("Windows", "None", "ModelReader", 1802),
    "Default.xnb": ("Windows", "None", "SpriteFontReader", 21678),
    "SampleCube64DXT1Mips.xnb": ("Windows", "None", "TextureCubeReader", 16813),
    "tone_mono_44khz_16bit.xnb": ("DesktopGL", "None", "SoundEffectReader", 44205),
    "one_two_three.xnb": ("DesktopGL", "None", "SongReader", 131),
    "Explosion.xnb": ("Windows", "LZX", "Texture2DReader", 1614),
    "FontCalibri14.xnb": ("Windows", "LZX", "SpriteFontReader", 10850),
}
SAMPLE_SHA256 = {
    "BlenderDefaultCube.xnb": "6eee09d81999eb9b8be243c6cc3d9049d26864c54c1f4a501183d7b622c55af4",
    "Default.xnb": "9eedaed56f7c0580588141b24d22118e187f612f31729a9030f0278453aa55d3",
    "SampleCube64DXT1Mips.xnb": "b0c8eceb8a301bc9c5ce69cde864243cca7bb36381b14fb7a4be7d36e7289c07",
    "tone_mono_44khz_16bit.xnb": "2f929b1bec2400f21fb8f430f51040fe8d0dbf7b953cac2fc56407b45fbd915c",
    "one_two_three.xnb": "8d7637c03ef73b9f34dee2291b65bba1eec12991e7e9795d8fc345c4160531dc",
    "Explosion.xnb": "713d5a0136b685d928933f945edc687a4c677f293ebdf2d4302b3a3bfeb77a1c",
    "FontCalibri14.xnb": "037f16df9c218bbf3bde19207b241fb694fd409f3a742efb682672c17ec8cfe6",
}


def _read_resource(opener, name):
    for folder in range(opener.folder_count()):
        for index in range(opener.resource_count(folder)):
            resource = _HANDLE()
            assert opener._L.peare_opener_open_resource_at(
                opener._h, folder, index, resource) == 0
            try:
                context = _Context()
                assert opener._L.peare_resource_get_context(resource, context) == 0
                actual_name = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                size = int(context.data_size)
                opener._L.peare_resource_context_free(context)
                if actual_name != name:
                    continue
                data = bytearray()
                for offset in range(0, size, 4096):
                    chunk = opener._lib.read_resource(resource, offset, min(4096, size - offset))
                    assert chunk
                    data.extend(chunk)
                return bytes(data)
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"XNB resource not found: {name}")


def _tree_paths(opener):
    paths = [item["path"] for item in opener.normalized_tree(recurse_containers=False)]
    assert len(paths) == len(set(paths)), f"Duplicate resource paths: {paths}"
    return set(paths)


def test_xnb_abi_reads_mono_game_samples(peare_lib):
    for name, (platform, compression, root_reader, size) in CASES.items():
        sample = FIXTURES / name
        assert sample.stat().st_size == size
        assert hashlib.sha256(sample.read_bytes()).hexdigest() == SAMPLE_SHA256[name]
        with peare_lib.open_file(str(sample)) as opener:
            assert opener.container_format() == "XNB"
            paths = _tree_paths(opener)
            assert {"XNB metadata.txt", sample.stem + ".asset.bin"} <= paths
            assert paths - {"XNB metadata.txt", sample.stem + ".asset.bin"} in (
                set(), {sample.stem + ".texture.bmp"})
            metadata = _read_resource(opener, "XNB metadata.txt").decode("utf-8")
            assert f"Platform: {platform}" in metadata
            assert "XNB version: 5" in metadata
            assert f"Compression: {compression}" in metadata
            assert root_reader in metadata
            assert "Expanded content size:" in metadata
            payload = _read_resource(opener, sample.stem + ".asset.bin")
            assert payload


def test_xnb_abi_reads_monogame_lz4_sample(peare_lib):
    with peare_lib.open_file(str(LZ4_FIXTURE)) as opener:
        assert opener.container_format() == "XNB"
        metadata = _read_resource(opener, "XNB metadata.txt").decode("utf-8")
        assert "XNB version: 5" in metadata
        assert "Compression: LZ4" in metadata
        assert "Texture2DReader" in metadata
        asset = _read_resource(opener, "white-1.asset.bin")
        assert asset == struct.pack("<IIIII", 0, 1, 1, 1, 4) + b"\xff" * 4
        preview = _read_resource(opener, "white-1.texture.bmp")
        assert preview[:2] == b"BM"
        assert struct.unpack_from("<I", preview, 2)[0] == len(preview)


def test_xnb_abi_reads_xna_31_version_4_sample(peare_lib):
    with peare_lib.open_file(str(V4_FIXTURE)) as opener:
        assert opener.container_format() == "XNB"
        metadata = _read_resource(opener, "XNB metadata.txt").decode("utf-8")
        assert "Platform: Windows" in metadata
        assert "XNB version: 4" in metadata
        assert "Compression: None" in metadata
        assert "Texture2DReader" in metadata
        asset = _read_resource(opener, "LogoOnly_64px-v4.asset.bin")
        assert len(asset) == 64 * 64 * 4 + 20
        preview = _read_resource(opener, "LogoOnly_64px-v4.texture.bmp")
        assert preview[:2] == b"BM"
        assert struct.unpack_from("<ii", preview, 18) == (64, -64)
        assert struct.unpack_from("<I", preview, 2)[0] == len(preview)


def test_xnb_abi_opens_all_monogame_test_assets(peare_lib):
    samples = sorted((ROOT / "tests" / "fixtures" / "xnb-monogame-upstream")
                     .rglob("*.xnb"))
    assert len(samples) == 126
    for sample in samples:
        with peare_lib.open_file(str(sample)) as opener:
            assert opener.container_format() == "XNB", sample
            paths = _tree_paths(opener)
            assert {"XNB metadata.txt", sample.stem + ".asset.bin"} <= paths, sample
            assert paths - {"XNB metadata.txt", sample.stem + ".asset.bin"} in (
                set(), {sample.stem + ".texture.bmp"}), sample
            metadata = _read_resource(opener, "XNB metadata.txt").decode("utf-8")
            assert "XNB version: " in metadata, sample
            assert "Content readers:" in metadata, sample
            root_reader = metadata.split("Root reader: ", 1)[1].splitlines()[0]
            payload = _read_resource(opener, sample.stem + ".asset.bin")
            assert payload or root_reader == "Null", sample
            if sample.stem + ".texture.bmp" in paths:
                image = _read_resource(opener, sample.stem + ".texture.bmp")
                assert image[:2] == b"BM", sample
                width, height = struct.unpack_from("<ii", image, 18)
                assert width > 0 and height < 0, sample
                assert struct.unpack_from("<I", image, 2)[0] == len(image), sample


def test_xnb_gui_preview_shows_compiled_asset_metadata(peare_gui_path):
    sample = FIXTURES / "Explosion.xnb"
    output = ROOT / "tests" / "output" / "xnb" / "explosion-metadata.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    plugin_name = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    candidates = [ROOT / "build-windows" / "test-plugins" / "platforms",
                  peare_gui_path.parent / "platforms"]
    plugin_dir = next((path for path in candidates if (path / plugin_name).is_file()), candidates[-1])
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run([
        str(peare_gui_path), "--preview-resource", str(sample), "XNB metadata.txt", str(output),
    ], capture_output=True, text=True, env=environment, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"]
    assert inspection["size"] > 0
    assert not inspection.get("duplicate_paths")
    assert valid_png(output)


def test_xnb_gui_previews_mono_game_texture(peare_gui_path):
    sample = FIXTURES / "Explosion.xnb"
    output = ROOT / "tests" / "output" / "xnb" / "explosion-texture.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    plugin_name = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    candidates = [ROOT / "build-windows" / "test-plugins" / "platforms",
                  peare_gui_path.parent / "platforms"]
    plugin_dir = next((path for path in candidates if (path / plugin_name).is_file()), candidates[-1])
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run([
        str(peare_gui_path), "--preview-resource", str(sample),
        "Explosion.texture.bmp", str(output),
    ], capture_output=True, text=True, env=environment, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"]
    assert inspection["size"] == 70 + 64 * 64 * 4
    assert not inspection.get("duplicate_paths")
    assert valid_png(output)


def test_xnb_gui_previews_xna_31_texture(peare_gui_path):
    output = ROOT / "tests" / "output" / "xnb" / "xna-31-texture.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    plugin_name = "qoffscreen.dll" if os.name == "nt" else (
        "libqoffscreen.dylib" if sys.platform == "darwin" else "libqoffscreen.so")
    candidates = [ROOT / "build-windows" / "test-plugins" / "platforms",
                  ROOT / "build-windows" / "Release" / "platforms"]
    plugin_dir = next((path for path in candidates if (path / plugin_name).is_file()), candidates[-1])
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    result = subprocess.run([
        str(peare_gui_path), "--preview-resource", str(V4_FIXTURE),
        "LogoOnly_64px-v4.texture.bmp", str(output),
    ], capture_output=True, text=True, env=environment, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r"^GUI_INSPECTION (\{.*\})$", result.stdout, re.MULTILINE)
    assert match, result.stdout
    inspection = json.loads(match.group(1))
    assert inspection["leaf"]
    assert inspection["size"] == 70 + 64 * 64 * 4
    assert not inspection.get("duplicate_paths")
    assert valid_png(output)
