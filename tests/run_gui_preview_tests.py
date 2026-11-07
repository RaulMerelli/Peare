import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

IMAGE_COUNT = re.compile(r"\((\d+) images,")
REPO_ROOT = Path(__file__).resolve().parents[1]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run format-independent offscreen previews in the Peare GUI.")
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path,
                        default=REPO_ROOT / "tmp/test-output/gui-previews")
    parser.add_argument("--qt-platform-plugin-dir", type=Path)
    parser.add_argument("--timeout", type=int, default=120)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--case", action="append", nargs=3,
                        metavar=("NAME", "SOURCE", "TREE_PATH"))
    inputs.add_argument("--manifest", help="JSON case list, or '-' for standard input")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args()


def valid_png(path):
    try:
        with path.open("rb") as stream:
            header = stream.read(24)
    except OSError:
        return False
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        return False
    width, height = struct.unpack(">II", header[16:24])
    return width >= 640 and height >= 400


def run_case(executable, output_dir, timeout, case, child_env):
    name = case["name"]
    source_text = case["source"]
    tree_path = case["tree_path"]
    min_images = int(case.get("min_images", 0))
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        raise ValueError(f"invalid case name: {name}")
    if min_images < 0:
        raise ValueError(f"invalid minimum image count: {min_images}")
    source = Path(source_text).resolve()
    if not source.exists():
        raise FileNotFoundError(f"source does not exist: {source}")

    screenshot = output_dir / f"{name}.png"
    command = [str(executable), "--preview-resource", str(source), tree_path,
               str(screenshot)]
    if "password" in case:
        child_env["PEARE_PREVIEW_PASSWORD"] = str(case["password"])
    else:
        child_env.pop("PEARE_PREVIEW_PASSWORD", None)
    result = subprocess.run(command, capture_output=True, text=True, env=child_env,
                            timeout=timeout, check=False)
    if result.returncode != 0:
        details = (result.stdout + result.stderr).strip()
        raise RuntimeError(f"{name}: Peare exited {result.returncode}: {details}")
    match = IMAGE_COUNT.search(result.stdout)
    if not match:
        raise RuntimeError(f"{name}: Peare did not report preview image metrics")
    image_count = int(match.group(1))
    if image_count < min_images:
        raise RuntimeError(
            f"{name}: expected at least {min_images} preview images, got {image_count}")
    if not valid_png(screenshot):
        raise RuntimeError(f"{name}: Peare did not save a valid GUI screenshot")
    return {"name": name, "sample_id": case.get("sample_id", str(source)),
            "source": str(source), "tree_path": tree_path,
            "images": image_count, "screenshot": str(screenshot), "passed": True}


def load_cases(args):
    if args.case:
        return [dict(name=name, source=source, tree_path=tree_path)
                for name, source, tree_path in args.case]
    data = sys.stdin.read() if args.manifest == "-" else Path(args.manifest).read_text(
        encoding="utf-8")
    cases = json.loads(data)
    if not isinstance(cases, list) or any(not isinstance(case, dict) for case in cases):
        raise ValueError("manifest must contain a JSON array of case objects")
    for case in cases:
        if not all(key in case for key in ("name", "source", "tree_path")):
            raise ValueError("each case needs name, source, and tree_path")
    return cases


def main():
    args = parse_args()
    executable = args.exe.resolve()
    if not executable.is_file():
        print(f"Peare executable does not exist: {executable}", file=sys.stderr)
        return 2
    plugin_name = ("qoffscreen.dll" if sys.platform == "win32" else
                   "libqoffscreen.dylib" if sys.platform == "darwin" else
                   "libqoffscreen.so")
    plugin_dir = args.qt_platform_plugin_dir
    if plugin_dir is None:
        configured = os.environ.get("QT_QPA_PLATFORM_PLUGIN_PATH")
        if configured:
            plugin_dir = Path(configured)
        else:
            candidates = [REPO_ROOT / "build-windows" / "test-plugins" / "platforms",
                          executable.parent / "platforms"]
            plugin_dir = next((path for path in candidates
                               if (path / plugin_name).is_file()), candidates[-1])
    plugin_dir = plugin_dir.resolve()
    if not (plugin_dir / plugin_name).is_file():
        print(f"Offscreen Qt platform plugin not found: {plugin_dir / plugin_name}",
              file=sys.stderr)
        return 2
    if args.jobs < 1:
        print("--jobs must be greater than zero", file=sys.stderr)
        return 2
    args.output_dir.mkdir(parents=True, exist_ok=True)

    try:
        cases = load_cases(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Could not load GUI preview cases: {error}", file=sys.stderr)
        return 2

    results = []
    child_env = dict(os.environ)
    child_env["QT_QPA_PLATFORM"] = "offscreen"
    child_env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    def execute(case):
        case_env = dict(child_env)
        if case.get("scroll_bottom"):
            case_env["PEARE_PREVIEW_SCROLL_BOTTOM"] = "1"
        try:
            return run_case(executable, args.output_dir.resolve(), args.timeout,
                            case, case_env)
        except (FileNotFoundError, OSError, RuntimeError, ValueError,
                subprocess.TimeoutExpired) as error:
            return {"name": case.get("name", "<unnamed>"),
                    "sample_id": case.get("sample_id", case.get("source", "")),
                    "source": case.get("source", ""),
                    "tree_path": case.get("tree_path", ""),
                    "error": str(error), "passed": False}

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(execute, cases):
            results.append(result)
            if not args.quiet or (not result["passed"] and
                                  sum(not item["passed"] for item in results) <= 25):
                state = "PASS" if result["passed"] else "FAIL"
                details = f": {result['error']}" if not result["passed"] else ""
                print(f"{state} {result['name']}{details}")
    if args.quiet and (failed_cases := sum(not result["passed"] for result in results)):
        print("Further failures are recorded in the detailed report.")

    samples = {}
    for result in results:
        samples.setdefault(result["sample_id"], []).append(result["passed"])
    passed_samples = sum(all(statuses) for statuses in samples.values())
    failed_samples = len(samples) - passed_samples
    passed_cases = sum(result["passed"] for result in results)
    failed_cases = len(results) - passed_cases
    print(f"GUI preview source samples: {len(samples)} total, {passed_samples} passed, "
          f"{failed_samples} failed.")
    print(f"GUI preview resource cases: {len(results)} total, {passed_cases} passed, "
          f"{failed_cases} failed.")
    if args.report:
        report = args.report.resolve()
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"Detailed report: {report}")
    return 1 if failed_cases else 0


if __name__ == "__main__":
    raise SystemExit(main())
