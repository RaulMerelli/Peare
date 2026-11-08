import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

from conftest import ROOT
from peare_opener import _Context, _HANDLE


FIXTURE = ROOT / "tests" / "fixtures" / "dxbc" / "postprocess-copy.cso"
FIXTURE_SHA256 = "5d3abdb41d6ef3452662d22c4781024eb89f1a14ff9fb6fff822e0a0088df7bf"
CHUNKS = {
    "Chunk 00 RDEF.bin": (148, "e7461c1fd2c068ac0bfe45a078eb0267541a691f0e436f18a3c70ffb7b16c5b9"),
    "Chunk 01 ISGN.bin": (80, "d0f60efb5cef3d5f8c1b1a778713c0898700d3397fe15b2bfc08f22ecf6b8081"),
    "Chunk 02 OSGN.bin": (44, "cf466b6b68fea77696a6c3adfff65fc3d1e474f7050916d1e868ad29939d16df"),
    "Chunk 03 SHDR.bin": (100, "37b4d2f1adc9ed409cbc31e830c2df379be150028af0bfac7a2fd71455402c8b"),
    "Chunk 04 STAT.bin": (116, "6ede7d03cf700739cf7a729c59ad9e70219b4cbf732e54b7e67b97d0163fbc55"),
}


def _tree_paths(opener):
    paths = [item["path"] for item in opener.normalized_tree(recurse_containers=False)]
    assert len(paths) == len(set(paths)), f"Duplicate resource paths: {paths}"
    return set(paths)


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
                if actual_name == name:
                    data = bytearray()
                    for offset in range(0, size, 4096):
                        data.extend(opener._lib.read_resource(
                            resource, offset, min(4096, size - offset)))
                    return bytes(data)
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"DXBC resource not found: {name}")


def test_dxbc_abi_exposes_validated_container_and_chunks(peare_lib):
    source = FIXTURE.read_bytes()
    assert len(source) == 580
    assert hashlib.sha256(source).hexdigest() == FIXTURE_SHA256
    with peare_lib.open_file(str(FIXTURE)) as opener:
        assert opener.container_format() == "DXBC"
        paths = _tree_paths(opener)
        assert paths == {"DXBC metadata.txt", *CHUNKS}
        metadata = _read_resource(opener, "DXBC metadata.txt").decode("utf-8")
        assert "Container version: 1" in metadata
        assert "Declared size: 580 bytes" in metadata
        assert "Chunk count: 5" in metadata
        assert "Checksum: present; not verified" in metadata
        for name, (size, digest) in CHUNKS.items():
            payload = _read_resource(opener, name)
            assert len(payload) == size
            assert hashlib.sha256(payload).hexdigest() == digest


def test_dxbc_gui_preview_opens_metadata_and_shader_chunk(peare_gui_path):
    output_dir = ROOT / "tests" / "output" / "dxbc-gui"
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = [
        {"name": "dxbc-metadata", "source": str(FIXTURE),
         "tree_path": "DXBC metadata.txt", "expected_size": 332,
         "expected_sha256": "ab2de2efae6a2707d126e6b5029f697c98d7809c63ef3adaaf5ca6edd660af2c",
         "expect_no_images": True},
        {"name": "dxbc-shader", "source": str(FIXTURE),
         "tree_path": "Chunk 03 SHDR.bin", "expected_size": 100,
         "expected_sha256": CHUNKS["Chunk 03 SHDR.bin"][1],
         "expect_no_images": True},
    ]
    manifest = output_dir / "manifest.json"
    manifest.write_text(json.dumps(cases), encoding="utf-8")
    command = [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
               "--exe", str(peare_gui_path), "--output-dir", str(output_dir),
               "--manifest", str(manifest), "--report", str(output_dir / "report.json")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120,
                            check=False, env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "2 total, 2 passed, 0 failed." in result.stdout, result.stdout
