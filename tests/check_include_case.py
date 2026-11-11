"""Detect case-mismatched quoted includes that Windows can hide."""

from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT / "src"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def resolve_case_from_index(source, relative, tracked):
    current = source.parent.relative_to(ROOT).parts
    mismatches = []
    parts = Path(relative.replace("/", "\\")).parts
    for part in parts:
        if part == ".":
            continue
        if part == "..":
            current = current[:-1]
            continue
        parent = "/".join(current).casefold()
        actual = tracked.get((parent, part.casefold()))
        if actual is None:
            return None
        if actual != part:
            mismatches.append((part, actual))
        current = current + (actual,)
    target = "/".join(current)
    return mismatches if target in tracked["files"] else None


def main():
    failures = []
    output = subprocess.run(
        ["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True)
    files = {item.decode("utf-8") for item in output.stdout.split(b"\0") if item}
    tracked = {"files": files}
    for name in files:
        parent, _, leaf = name.rpartition("/")
        tracked[(parent.casefold(), leaf.casefold())] = leaf
    for source in SOURCE_ROOT.rglob("*"):
        if source.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        try:
            lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as error:
            print(f"Cannot read {source}: {error}", file=sys.stderr)
            return 2
        for number, line in enumerate(lines, 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            relative = match.group(1).replace("/", "\\")
            mismatches = resolve_case_from_index(source, relative, tracked)
            if mismatches:
                failures.append((source, number, relative, mismatches))

    for source, number, relative, mismatches in failures:
        correction = relative
        for requested, actual in mismatches:
            correction = correction.replace(requested, actual)
        print(f"{source.relative_to(ROOT)}:{number}: {relative} -> {correction}")
    if failures:
        return 1
    print("Source-relative quoted include case is consistent.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
