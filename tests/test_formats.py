"""Golden-master tests: open each fixture through the C ABI and compare the
normalized resource tree against a committed `expected/<name>.json`.

The expected manifest is the golden master. A behavioural change makes a test
fail; you then either fix the regression or, if the change is intended,
regenerate the manifest and review the diff:

    PEARE_UPDATE_EXPECTED=1 pytest tests/test_formats.py

Payloads are compared by sha256 (recorded in the manifest), so a single wrong
byte in any extracted file fails the test while keeping manifests small.
"""

import json
import os
import ctypes as C
import hashlib
import struct
import subprocess
from pathlib import Path

import pytest
import peare_opener as po

TESTS = Path(__file__).resolve().parent
FORMATS = TESTS / "formats"

# Every file in a format's fixtures/ is an entry point to open, EXCEPT:
#  - sidecar files of a multi-file fixture (e.g. a VMDK descriptor's -flat extent),
#  - documentation (README / licence notes).
# Add a fixture only when its origin is recorded in the fixture documentation.
_SKIP_NAMES = ("-flat.vmdk", "store.bin")
_SKIP_SUFFIXES = {".md", ".txt"}


def _is_fixture(fx):
    return (fx.is_file()
            and fx.suffix.lower() not in _SKIP_SUFFIXES
            and not any(fx.name.endswith(s) for s in _SKIP_NAMES))


def _discover():
    cases = []
    if not FORMATS.is_dir():
        return cases
    for fmt_dir in sorted(FORMATS.iterdir()):
        fixtures = fmt_dir / "fixtures"
        if not fixtures.is_dir():
            continue
        for fx in sorted(fixtures.iterdir()):
            if _is_fixture(fx):
                cases.append(pytest.param(fx, id=f"{fmt_dir.name}/{fx.name}"))
    return cases


def _expected_path(fixture):
    same_stem = [item for item in fixture.parent.iterdir()
                 if _is_fixture(item) and item.stem == fixture.stem]
    name = fixture.stem if len(same_stem) == 1 else fixture.name
    return fixture.parent.parent / "expected" / (name + ".json")


@pytest.mark.parametrize("fixture", _discover())
def test_fixture_matches_golden(peare_lib, fixture):
    opener = peare_lib.open_file(str(fixture))
    assert opener is not None, f"opener failed to open {fixture}"
    try:
        actual = opener.snapshot()
    finally:
        opener.close()

    expected_path = _expected_path(fixture)

    if os.environ.get("PEARE_UPDATE_EXPECTED") == "1":
        expected_path.parent.mkdir(parents=True, exist_ok=True)
        expected_path.write_text(json.dumps(actual, indent=2, ensure_ascii=False) + "\n",
                                 encoding="utf-8")
        pytest.skip(f"updated golden {expected_path.name}")

    assert expected_path.is_file(), (
        f"missing golden {expected_path}; create it with "
        f"PEARE_UPDATE_EXPECTED=1 and review the diff")
    expected = json.loads(expected_path.read_text(encoding="utf-8"))
    assert actual == expected


def test_qemu_empty_stream_optimized_vmdk_is_readable(peare_lib, tmp_path):
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("set PEARE_QEMU_IMG to qemu-img")

    image = tmp_path / "qemu-empty-stream-optimized.vmdk"
    created = subprocess.run(
        [qemu_img, "create", "-f", "vmdk", "-o",
         "subformat=streamOptimized", str(image), "1G"],
        capture_output=True, text=True, timeout=120)
    assert created.returncode == 0, created.stdout + created.stderr

    header = image.read_bytes()[:512]
    assert header[:4] == b"KDMV"
    assert struct.unpack_from("<H", header, 77)[0] == 1

    info = subprocess.run(
        [qemu_img, "info", "--output=json", str(image)],
        capture_output=True, text=True, timeout=120)
    assert info.returncode == 0, info.stdout + info.stderr
    qemu_size = json.loads(info.stdout)["virtual-size"]

    opener = peare_lib.open_file(str(image))
    assert opener is not None, "opener rejected an empty QEMU streamOptimized VMDK"
    try:
        assert opener.container_format() == "VMDK"
        rows = opener.normalized_tree()
        assert len(rows) == 1
        assert rows[0]["path"] == "Whole disk"
        assert rows[0]["size"] == qemu_size
        assert rows[0]["is_container"] is False
    finally:
        opener.close()


def test_qemu_nonempty_stream_optimized_vmdk_preserves_disk_bytes(peare_lib, tmp_path):
    qemu_img = os.environ.get("PEARE_QEMU_IMG")
    if not qemu_img or not Path(qemu_img).is_file():
        pytest.skip("set PEARE_QEMU_IMG to qemu-img")
    qemu_io = Path(os.environ.get(
        "PEARE_QEMU_IO", str(Path(qemu_img).with_name("qemu-io" + Path(qemu_img).suffix))))
    if not qemu_io.is_file():
        pytest.skip("set PEARE_QEMU_IO to qemu-io")

    source = tmp_path / "qemu-source.raw"
    image = tmp_path / "qemu-nonempty-stream-optimized.vmdk"
    created = subprocess.run(
        [qemu_img, "create", "-f", "raw", str(source), "2M"],
        capture_output=True, text=True, timeout=120)
    assert created.returncode == 0, created.stdout + created.stderr
    written = subprocess.run(
        [str(qemu_io), "-f", "raw", "-c", "write -P 0x5a 512 512", str(source)],
        capture_output=True, text=True, timeout=120)
    assert written.returncode == 0, written.stdout + written.stderr
    converted = subprocess.run(
        [qemu_img, "convert", "-f", "raw", "-O", "vmdk", "-o",
         "subformat=streamOptimized", str(source), str(image)],
        capture_output=True, text=True, timeout=120)
    assert converted.returncode == 0, converted.stdout + converted.stderr

    opener = peare_lib.open_file(str(image))
    assert opener is not None, "opener rejected a non-empty QEMU streamOptimized VMDK"
    try:
        rows = opener.normalized_tree()
        assert len(rows) == 1
        assert rows[0]["path"] == "Whole disk"
        assert rows[0]["sha256"] == hashlib.sha256(source.read_bytes()).hexdigest()
    finally:
        opener.close()


def _open_named_child(parent, name, required_format=None):
    api = parent._L
    for folder in range(parent.folder_count()):
        for index in range(parent.resource_count(folder)):
            resource = po._HANDLE()
            if api.peare_opener_open_resource_at(
                    parent._h, folder, index, C.byref(resource)) != 0:
                continue
            context = po._Context()
            if api.peare_resource_get_context(resource, C.byref(context)) != 0:
                api.peare_resource_destroy(resource)
                continue
            identifier = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
            format_name = po.CONTAINER_NAMES.get(context.container_format, "")
            is_container = bool(context.is_container)
            api.peare_resource_context_free(C.byref(context))
            matches = identifier.lower() == name.lower()
            if required_format:
                matches = matches and format_name == required_format
            if matches and is_container:
                source = po._HANDLE()
                status = api.peare_resource_get_source(resource, C.byref(source))
                api.peare_resource_destroy(resource)
                if status != 0:
                    return None
                child = parent._lib._open_source(source)
                api.peare_source_destroy(source)
                return child
            api.peare_resource_destroy(resource)
    return None


def test_ntfs_wof_file_provider_sample(peare_lib):
    image = os.environ.get("PEARE_NTFS_WOF_TEST_IMAGE")
    if not image:
        pytest.skip("set PEARE_NTFS_WOF_TEST_IMAGE to a VMDK with a WOF file")
    path = os.environ.get("PEARE_NTFS_WOF_TEST_PATH", "Windows/servicing/CbsMsg.dll")
    expected_hash = os.environ.get("PEARE_NTFS_WOF_TEST_SHA256")
    opener = peare_lib.open_file(image)
    assert opener is not None
    volume = None
    try:
        for folder in range(opener.folder_count()):
            for index in range(opener.resource_count(folder)):
                resource = po._HANDLE()
                if opener._L.peare_opener_open_resource_at(
                        opener._h, folder, index, C.byref(resource)) != 0:
                    continue
                context = po._Context()
                if opener._L.peare_resource_get_context(resource, C.byref(context)) != 0:
                    opener._L.peare_resource_destroy(resource)
                    continue
                is_ntfs = (context.container_format ==
                           next((key for key, value in po.CONTAINER_NAMES.items()
                                 if value == "NTFS"), -1))
                is_container = bool(context.is_container)
                opener._L.peare_resource_context_free(C.byref(context))
                if is_ntfs and is_container:
                    source = po._HANDLE()
                    status = opener._L.peare_resource_get_source(resource, C.byref(source))
                    opener._L.peare_resource_destroy(resource)
                    if status == 0:
                        volume = opener._lib._open_source(source)
                        opener._L.peare_source_destroy(source)
                        break
                else:
                    opener._L.peare_resource_destroy(resource)
            if volume:
                break
        assert volume is not None, "VMDK has no NTFS partition"
        parts = path.replace("\\", "/").split("/")
        current = volume
        for component in parts[:-1]:
            child = _open_named_child(current, component, "NTFS")
            if current is not volume:
                current.close()
            assert child is not None, f"NTFS directory not found: {component}"
            current = child
        target = None
        for folder in range(current.folder_count()):
            for index in range(current.resource_count(folder)):
                resource = po._HANDLE()
                if current._L.peare_opener_open_resource_at(
                        current._h, folder, index, C.byref(resource)) != 0:
                    continue
                context = po._Context()
                if current._L.peare_resource_get_context(resource, C.byref(context)) != 0:
                    current._L.peare_resource_destroy(resource)
                    continue
                identifier = context.identifier_utf8.to_bytes().decode("utf-8", "replace")
                current._L.peare_resource_context_free(C.byref(context))
                if identifier.lower() == parts[-1].lower():
                    blob = po._Blob()
                    if current._L.peare_resource_get_payload(resource, C.byref(blob)) == 0:
                        target = blob.to_bytes()
                        current._L.peare_blob_free(C.byref(blob))
                    current._L.peare_resource_destroy(resource)
                    break
                current._L.peare_resource_destroy(resource)
            if target is not None:
                break
        assert target, f"NTFS file not found or empty: {path}"
        assert target[:2] == b"MZ"
        assert any(target)
        if expected_hash:
            assert hashlib.sha256(target).hexdigest().lower() == expected_hash.lower()
    finally:
        if volume:
            volume.close()
        opener.close()
