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
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
FORMATS = TESTS / "formats"

# Every file in a format's fixtures/ is an entry point to open, EXCEPT:
#  - sidecar files of a multi-file fixture (e.g. a VMDK descriptor's -flat extent),
#  - documentation (README / licence notes).
# This way a new format is covered just by dropping its fixture in place.
_SKIP_NAMES = ("-flat.vmdk",)
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


@pytest.mark.parametrize("fixture", _discover())
def test_fixture_matches_golden(peare_lib, fixture):
    opener = peare_lib.open_file(str(fixture))
    assert opener is not None, f"opener failed to open {fixture}"
    try:
        actual = opener.snapshot()
    finally:
        opener.close()

    expected_path = fixture.parent.parent / "expected" / (fixture.stem + ".json")

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
