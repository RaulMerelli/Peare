import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

from conftest import ROOT
from peare_opener import _Context, _HANDLE


FIXTURE = ROOT / "tests" / "fixtures" / "nup" / "reference.nup"
EXPECTED = {
    "__init__.py": (95, "fdc7711757d38bff2b0c235edc13b89497d99da9dfa4c60534d33fdf1dbef469"),
    "LICENSE.txt": (1084, "3469968dd0e1532f2ce44c637af40a671799af95242cf48e7061443f30144582"),
    "README.md": (5898, "d5990888011af1ad7c5148f671355cecc11aa140824d1e830a8dfec54f1a0543"),
    "format.md": (19923, "148d1de4af977880a60d0e6d96b2961843a1b34a72160b342d8ffd33b5cc6e7f"),
    "addon.md": (5898, "d5990888011af1ad7c5148f671355cecc11aa140824d1e830a8dfec54f1a0543"),
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
                for offset in range(0, size, 65536):
                    chunk = opener._lib.read_resource(resource, offset, min(65536, size - offset))
                    assert chunk
                    data.extend(chunk)
                return bytes(data)
            finally:
                opener._L.peare_resource_destroy(resource)
    raise AssertionError(f"NUP resource not found: {name}")


def _tree_paths(opener):
    paths = [item["path"] for item in opener.normalized_tree(recurse_containers=False)]
    assert len(paths) == len(set(paths)), f"Duplicate NUP paths: {paths}"
    return set(paths)


def test_nup_abi_reads_base_update_addon_and_zstd_payloads(peare_lib):
    source = FIXTURE.read_bytes()
    assert hashlib.sha256(source).hexdigest() == (
        "3b06f05be929fc1fd0554c0fe2cbdc321f75d118b45861fd8d0de27cc8494764")
    with peare_lib.open_file(str(FIXTURE)) as opener:
        assert opener.container_format() == "NUP"
        paths = _tree_paths(opener)
        assert len(paths) == 13
        assert "NUP metadata.txt" in paths
        assert any(" - NUP reference test/" in path for path in paths)
        assert all(any(path.endswith("/" + name) for path in paths) for name in EXPECTED)
        assert sum(" - base - " in path for path in paths) >= 1
        assert sum(" - update - " in path for path in paths) >= 1
        assert sum(" - add-on - " in path for path in paths) >= 1
        metadata = _read_resource(opener, "NUP metadata.txt").decode("utf-8")
        assert "Packages: 3" in metadata
        assert "TOC SHA-256: verified" in metadata
        for name, (size, digest) in EXPECTED.items():
            payload = _read_resource(opener, name)
            assert len(payload) == size
            assert hashlib.sha256(payload).hexdigest() == digest


def test_nup_gui_preview_opens_metadata_and_member(peare_gui_path):
    output_dir = ROOT / "tests" / "output" / "nup-gui"
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = [
        {"name": "nup-metadata", "source": str(FIXTURE),
         "tree_path": "NUP metadata.txt", "expected_size": 120,
         "expected_sha256": "a26eaaf484f0b9337cab6849b28edbee9f7be26457548c38ef28b5a69b82e1fd",
         "expect_no_images": True},
        {"name": "nup-raw-member", "source": str(FIXTURE),
         "tree_path": "Package 00 - base - 0100000000000000 - NUP reference test/romfs/config/__init__.py",
         "expected_size": EXPECTED["__init__.py"][0],
         "expected_sha256": EXPECTED["__init__.py"][1], "expect_no_images": True},
        {"name": "nup-zstd-member", "source": str(FIXTURE),
         "tree_path": "Package 00 - base - 0100000000000000 - NUP reference test/romfs/docs/README.md",
         "expected_size": EXPECTED["README.md"][0],
         "expected_sha256": EXPECTED["README.md"][1], "expect_no_images": True},
    ]
    manifest = output_dir / "manifest.json"
    manifest.write_text(json.dumps(cases), encoding="utf-8")
    command = [sys.executable, str(ROOT / "tests" / "run_gui_preview_tests.py"),
               "--exe", str(peare_gui_path), "--output-dir", str(output_dir),
               "--manifest", str(manifest), "--report", str(output_dir / "report.json")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120,
                            check=False, env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "3 total, 3 passed, 0 failed." in result.stdout, result.stdout
