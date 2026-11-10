import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

REPO_ROOT = Path(__file__).resolve().parents[1]


def process_memory(process):
    if sys.platform == "win32":
        from ctypes import wintypes

        class MemoryCounters(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t),
            ]

        counters = MemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if ctypes.windll.psapi.GetProcessMemoryInfo(
                wintypes.HANDLE(process._handle), ctypes.byref(counters),
                counters.cb):
            return {"working_set_bytes": counters.WorkingSetSize,
                    "private_bytes": counters.PrivateUsage}
    elif sys.platform.startswith("linux"):
        try:
            status = Path(f"/proc/{process.pid}/status").read_text()
            values = {}
            for line in status.splitlines():
                if line.startswith(("VmRSS:", "VmHWM:")):
                    key, value, _ = line.split(maxsplit=2)
                    values[key.rstrip(":").lower() + "_bytes"] = int(value) * 1024
            return values
        except (OSError, ValueError):
            return None
    return None


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.digest()


def copy_image(sample, output_dir):
    if sample.suffix.casefold() == ".vmdk":
        with sample.open("rb") as stream:
            descriptor_header = stream.read(1024)
        if b"# Disk DescriptorFile" in descriptor_header:
            descriptor_dir = output_dir / "writer-test-copy"
            descriptor_dir.mkdir(parents=True, exist_ok=True)
            descriptor = descriptor_dir / sample.name
            shutil.copy2(sample, descriptor)
            text = sample.read_text(encoding="latin1")
            for line in text.splitlines():
                match = re.match(
                    r'^(?:RW|RDONLY|NOACCESS)\s+\d+\s+\S+\s+"([^"]+)"',
                    line.strip())
                if not match:
                    continue
                relative_extent = Path(match.group(1))
                if relative_extent.is_absolute():
                    raise ValueError("VMDK extents must use relative paths.")
                extent = sample.parent / relative_extent
                if not extent.is_file():
                    raise FileNotFoundError(f"VMDK extent not found: {extent}")
                copy_target = (descriptor_dir / relative_extent).resolve()
                if not copy_target.is_relative_to(descriptor_dir.resolve()):
                    raise ValueError("VMDK extent path escapes the disposable directory.")
                copy_target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(extent, copy_target)
            return descriptor
    copied_image = output_dir / ("writer-test-copy" + sample.suffix)
    shutil.copy2(sample, copied_image)
    return copied_image


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run a generic offscreen CUD scenario through the Peare GUI.")
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--scenario", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path,
                        default=REPO_ROOT / "tmp/test-output/gui-writer")
    parser.add_argument("--qt-platform-plugin-dir", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    return parser.parse_args()


def main():
    args = parse_args()
    executable = args.exe.resolve()
    sample = args.image.resolve()
    scenario = args.scenario.resolve()
    output_dir = args.output_dir.resolve()
    if not executable.is_file() or not sample.is_file() or not scenario.is_file():
        print("Executable, image, or scenario file does not exist.", file=sys.stderr)
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
        print(f"Qt offscreen platform plugin not found: {plugin_dir / plugin_name}",
              file=sys.stderr)
        return 2

    output_dir.mkdir(parents=True, exist_ok=True)
    report = output_dir / "writer-test-report.json"
    screenshot = output_dir / "writer-test.png"
    original_hash = sha256(sample)
    copied_image = copy_image(sample, output_dir)
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(plugin_dir)
    environment["PEARE_ENABLE_GUI_WRITER_TEST"] = "1"
    command = [str(executable), "--gui-writer-test", str(copied_image),
               str(scenario), str(report), str(screenshot)]
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True,
                                   env=environment)
    except OSError as error:
        print(f"Could not start Peare: {error}", file=sys.stderr)
        return 1
    started = time.monotonic()
    memory_samples = []
    peak_memory = {}
    completed_count = -1
    timed_out = False
    while process.poll() is None:
        if time.monotonic() - started > args.timeout:
            timed_out = True
            process.kill()
            break
        memory = process_memory(process)
        if memory:
            for key, value in memory.items():
                peak_memory[key] = max(peak_memory.get(key, 0), value)
            if report.is_file():
                try:
                    progress = json.loads(report.read_text(encoding="utf-8"))
                    operations = progress.get("operations", [])
                    if len(operations) != completed_count:
                        completed_count = len(operations)
                        memory_samples.append({
                            "completed_operations": completed_count,
                            "last_test_id": operations[-1].get("test_id")
                                if operations else "startup",
                            **memory,
                        })
                except (OSError, json.JSONDecodeError):
                    pass
        time.sleep(0.1)
    stdout, stderr = process.communicate()
    result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    if timed_out:
        print(f"GUI writer scenario exceeded {args.timeout} seconds.", file=sys.stderr)
        if report.is_file():
            progress = json.loads(report.read_text(encoding="utf-8"))
            completed = len(progress.get("operations", []))
            active = progress.get("active_operation", "not recorded")
            print(f"Last GUI stage: {progress.get('stage', 'unknown')}; "
                  f"completed operations: {completed}; active: {active}",
                  file=sys.stderr)
        if sha256(sample) != original_hash:
            print("The original sample changed during GUI writer testing.", file=sys.stderr)
        return 1
    if sha256(sample) != original_hash:
        print("The original sample changed during GUI writer testing.", file=sys.stderr)
        return 1
    if result.returncode != 0:
        print((result.stdout + result.stderr).strip(), file=sys.stderr)
        if report.is_file():
            progress = json.loads(report.read_text(encoding="utf-8"))
            completed = len(progress.get("operations", []))
            active = progress.get("active_operation", "not recorded")
            print(f"GUI stage: {progress.get('stage', 'unknown')}; "
                  f"completed operations: {completed}; active: {active}",
                  file=sys.stderr)
        return result.returncode or 1
    if not report.is_file() or not screenshot.is_file():
        print("Peare did not produce the GUI report and screenshot.", file=sys.stderr)
        return 1
    with screenshot.open("rb") as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        print("Peare did not produce a valid GUI screenshot.", file=sys.stderr)
        return 1
    width, height = struct.unpack(">II", header[16:24])
    if width < 640 or height < 400:
        print("The GUI screenshot is smaller than the minimum test viewport.",
              file=sys.stderr)
        return 1
    data = json.loads(report.read_text(encoding="utf-8"))
    operations = data.get("operations", [])
    if not operations or not all(item.get("passed") for item in operations):
        print("GUI writer report contains a failed or empty operation list.",
              file=sys.stderr)
        return 1
    if peak_memory:
        memory_path = output_dir / "writer-test-memory.json"
        memory_path.write_text(json.dumps({"peak": peak_memory,
                                          "samples": memory_samples},
                                         indent=2), encoding="utf-8")
        print(f"GUI peak memory: {peak_memory}; samples: {len(memory_samples)}")
    print(f"GUI writer scenario: {len(operations)} operations passed; "
          "source sample unchanged.")
    print(f"Disposable image: {copied_image}")
    print(f"Report: {report}")
    print(f"Screenshot: {screenshot}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
