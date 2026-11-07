"""Check split WIM parts through the public Opener ABI."""

from pathlib import Path

import pytest


FIXTURES = Path(__file__).resolve().parent / "formats" / "wim" / "fixtures"
PAYLOADS = {
    "payload-1.bin": "654d2fba08fcb04660c46daa1323024545ebace16913533dd37b705726f6fcdc",
    "payload-2.bin": "69ef50559afeeb2b48695075ef95119373b49f3497caa26b0b34269e1177f1ba",
    "payload-3.bin": "e2a11780fc95d24db4867636e676048f69b8094b85db89a2d58e3b9d83aedfb4",
    "payload-4.bin": "a4ba7b8c701671de76fad7f13ca46560c1484e47c1c2e090b288d772e87848ac",
}


@pytest.mark.parametrize("part", ("test.swm", "test2.swm"))
def test_split_wim_parts_open_the_complete_image(peare_lib, part):
    opener = peare_lib.open_file(str(FIXTURES / part))
    assert opener is not None
    try:
        assert opener.container_format() == "WIM"
        rows = opener.normalized_tree()
        assert {row["path"] for row in rows} == set(PAYLOADS)
        assert all(row["size"] == 409600 for row in rows)
        assert {row["path"]: row["sha256"] for row in rows} == PAYLOADS
    finally:
        opener.close()
