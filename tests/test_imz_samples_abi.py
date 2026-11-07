import os
from pathlib import Path

import pytest


def test_imz_tree_matches_raw_floppy(peare_lib):
    imz = Path(os.environ.get("PEARE_IMZ_SAMPLE", ""))
    raw = Path(os.environ.get("PEARE_IMZ_RAW_SAMPLE", ""))
    if not imz.is_file() or not raw.is_file():
        pytest.skip("set PEARE_IMZ_SAMPLE and PEARE_IMZ_RAW_SAMPLE to local samples")

    compressed = peare_lib.open_file(str(imz))
    source = peare_lib.open_file(str(raw))
    assert compressed is not None, f"opener rejected {imz}"
    assert source is not None, f"opener rejected {raw}"
    try:
        assert compressed.container_format() == source.container_format()
        assert compressed.normalized_tree() == source.normalized_tree()
    finally:
        compressed.close()
        source.close()
