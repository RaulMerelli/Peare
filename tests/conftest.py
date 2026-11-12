"""pytest configuration: locate the built PeareOpener library and expose it.

The tests drive the *shipping artifact* (the .dll/.so) through the C ABI, so they
need to find it. Search order:

1. the PEARE_OPENER_LIB environment variable (an explicit path), then
2. the usual build output directories.

If no library is found, the ABI tests are skipped (not failed): a checkout without
a build should not report red.
"""

import os
import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

_NAMES = ["PeareOpener.dll", "libPeareOpener.so", "libPeareOpener.dylib"]
_DECODER_NAMES = ["PeareDecoder.dll", "libPeareDecoder.so", "libPeareDecoder.dylib"]
_GUI_NAMES = ["Peare.exe", "Peare"]
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


@pytest.fixture(scope="session")
def peare_decoder_path() -> Path:
    env = os.environ.get("PEARE_DECODER_LIB")
    if env and Path(env).is_file():
        return Path(env)
    for directory in _SEARCH_DIRS:
        for name in _DECODER_NAMES:
            path = ROOT / directory / name
            if path.is_file():
                return path
    for build in ROOT.glob("build*"):
        for name in _DECODER_NAMES:
            hits = list(build.rglob(name))
            if hits:
                return hits[0]
    pytest.skip("PeareDecoder library not found; build it or set PEARE_DECODER_LIB")


@pytest.fixture(scope="session")
def peare_gui_path() -> Path:
    env = os.environ.get("PEARE_GUI_EXE")
    if env and Path(env).is_file():
        return Path(env)
    for directory in _SEARCH_DIRS:
        for name in _GUI_NAMES:
            path = ROOT / directory / name
            if path.is_file():
                return path
    pytest.skip("Official Peare GUI executable not found; set PEARE_GUI_EXE")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    setattr(item, "rep_" + report.when, report)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_teardown(item, nextitem):
    yield
    report = getattr(item, "rep_call", None)
    tmp_path = item.funcargs.get("tmp_path")
    if report is not None and report.passed and tmp_path is not None:
        shutil.rmtree(str(tmp_path), ignore_errors=True)
