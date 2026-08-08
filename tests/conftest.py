"""pytest configuration: locate the built PeareOpener library and expose it.

The tests drive the *shipping artifact* (the .dll/.so) through the C ABI, so they
need to find it. Search order:

1. the PEARE_OPENER_LIB environment variable (an explicit path), then
2. the usual build output directories.

If no library is found, the ABI tests are skipped (not failed): a checkout without
a build should not report red.
"""

import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

_NAMES = ["PeareOpener.dll", "libPeareOpener.so", "libPeareOpener.dylib"]
_SEARCH_DIRS = [
    "build-windows/Release", "build-windows", "build-windows/lib/Release",
    "build/Release", "build", "build/lib",
    "build-linux", "build-linux/Release",
]


def _find_library() -> Path | None:
    env = os.environ.get("PEARE_OPENER_LIB")
    if env and Path(env).is_file():
        return Path(env)
    for d in _SEARCH_DIRS:
        for name in _NAMES:
            p = ROOT / d / name
            if p.is_file():
                return p
    # last resort: recursive search under build* directories
    for build in ROOT.glob("build*"):
        for name in _NAMES:
            hits = list(build.rglob(name))
            if hits:
                return hits[0]
    return None


@pytest.fixture(scope="session")
def peare_lib():
    path = _find_library()
    if path is None:
        pytest.skip("PeareOpener library not found; build it or set PEARE_OPENER_LIB")
    import peare_opener
    return peare_opener.load_library(path)
