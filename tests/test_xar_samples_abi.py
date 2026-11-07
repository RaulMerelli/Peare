"""Check XAR reference-tool samples through the public Opener ABI."""

import hashlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SAMPLES = REPO / "tests" / "formats" / "xar" / "fixtures"


def _tree(peare_lib, path):
    opener = peare_lib.open_file(str(path))
    assert opener is not None, f"Peare failed to open {path.name}"
    with opener:
        assert opener.container_format() == "XAR"
        return opener.snapshot()["tree"]


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def test_xar_mixed_sample_matches_reference_contents(peare_lib):
    sample = SAMPLES / "mixed.xar"
    store = SAMPLES / "store.bin"
    assert sample.is_file() and store.is_file()

    expected = {
        "plain.txt": (10, _digest(b"hello xar\n")),
        "nested/deep/file.txt": (20, _digest(b"nested content here\n")),
        "big.txt": (80000, _digest(b"abcdefgh" * 10000)),
        "setuid.sh": (7, _digest(b"setuid\n")),
        "store.bin": (store.stat().st_size, _digest(store.read_bytes())),
    }
    rows = _tree(peare_lib, sample)
    actual = {row["path"]: (row["size"], row["sha256"]) for row in rows if row["size"]}
    assert actual == expected
    assert any(row["path"].endswith("empty.txt") and row["size"] == 0 for row in rows)


def test_xar_bzip2_members_match_reference_contents(peare_lib):
    sample = SAMPLES / "bzip2.xar"
    assert sample.is_file()

    expected = {
        "plain.txt": (10, _digest(b"hello xar\n")),
        "nested/deep/file.txt": (20, _digest(b"nested content here\n")),
    }
    rows = _tree(peare_lib, sample)
    actual = {row["path"]: (row["size"], row["sha256"]) for row in rows if row["size"]}
    assert actual == expected
