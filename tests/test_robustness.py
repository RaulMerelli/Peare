"""Robustness: malformed input must fail gracefully, never crash the process.

For every fixture we feed the Opener truncated and garbage variants and only
require that the call returns (an opener or a clean failure) without a hard crash,
hang, or memory blow-up. This is the cheapest, highest-value guard for a codebase
where most readers are machine-generated: it catches the missing bounds check that
turns a weird real-world file into a segfault.

The child process isolates crashes: a native fault in the DLL takes down only the
worker, and the test reports it as a failure instead of killing pytest.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
FORMATS = TESTS / "formats"
_SKIP = ("-flat.vmdk",)
_SKIP_SUFFIXES = {".md", ".txt"}


def _fixtures():
    out = []
    if not FORMATS.is_dir():
        return out
    for fx in sorted(FORMATS.rglob("*")):
        if (fx.is_file() and "fixtures" in fx.parts
                and fx.suffix.lower() not in _SKIP_SUFFIXES
                and not any(fx.name.endswith(s) for s in _SKIP)):
            out.append(pytest.param(fx, id=str(fx.relative_to(FORMATS))))
    return out


# Worker: open a bytes blob written to a temp file; print OK/ERR; a crash shows as
# a non-zero exit / no output, which the parent turns into a failed assertion.
_WORKER = r"""
import sys
sys.path.insert(0, r"{tests}")
import peare_opener as po
lib = po.load_library(r"{lib}")
op = lib.open_file(sys.argv[1])
if op:
    try:
        op.normalized_tree(max_depth=3)
    finally:
        op.close()
print("OK")
"""


@pytest.mark.parametrize("fixture", _fixtures())
@pytest.mark.parametrize("mutation", ["truncate_half", "truncate_1k", "header_only", "garbage"])
def test_malformed_does_not_crash(peare_lib, fixture, mutation, tmp_path, request):
    lib_path = os.environ.get("PEARE_OPENER_LIB")
    if not lib_path:
        pytest.skip("set PEARE_OPENER_LIB so the worker can locate the library")

    data = fixture.read_bytes()
    if mutation == "truncate_half":
        blob = data[: len(data) // 2]
    elif mutation == "truncate_1k":
        blob = data[:1024]
    elif mutation == "header_only":
        blob = data[:512]
    else:  # garbage: keep length, scramble deterministically
        blob = bytes((b ^ 0x5A) for b in data)

    victim = tmp_path / (fixture.stem + fixture.suffix)
    victim.write_bytes(blob)

    src = _WORKER.format(tests=str(TESTS), lib=lib_path)
    proc = subprocess.run([sys.executable, "-c", src, str(victim)],
                          capture_output=True, timeout=60)
    assert proc.returncode == 0 and b"OK" in proc.stdout, (
        f"{fixture.name} [{mutation}] crashed the reader "
        f"(exit {proc.returncode}): {proc.stderr.decode('utf-8', 'replace')[:300]}")
