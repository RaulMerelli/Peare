"""Run pytest with all generated files inside the ignored project output folder."""

import os
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "tmp" / "test-output"
OUTPUT.mkdir(parents=True, exist_ok=True)
os.environ["PYTHONPYCACHEPREFIX"] = str(OUTPUT / "pycache")
sys.pycache_prefix = str(OUTPUT / "pycache")
os.chdir(ROOT)

import pytest


arguments = sys.argv[1:]
has_test_target = any(
    argument.endswith(".py") or "::" in argument
    for argument in arguments
)
if not has_test_target:
    arguments.insert(0, "tests")
arguments[:0] = ["-c", "tests/pytest.ini"]
raise SystemExit(pytest.main(arguments))
