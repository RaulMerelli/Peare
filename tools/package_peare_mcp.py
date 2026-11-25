"""Build self-contained Peare MCP downloads from a desktop Peare build."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from importlib.metadata import distribution
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO = "https://github.com/RaulMerelli/Peare"
WINDOWS = os.name == "nt"


def windows_runtime(package, runtime, qt, offscreen):
    native = package / "native"
    for name in ("PeareOpener", "PeareDecoder", "PeareWriter"):
        shutil.copy2(runtime / f"{name}.dll", native)
    for name in ("Core", "Gui", "Svg", "Widgets"):
        shutil.copy2(qt / "bin" / f"Qt5{name}.dll", native)
    platforms = native / "plugins" / "platforms"
    platforms.mkdir(parents=True)
    shutil.copy2(offscreen / "qoffscreen.dll", platforms)
    deploy_plugins(native, qt, "*.dll")
    vswhere = (Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
               / "Microsoft Visual Studio" / "Installer" / "vswhere.exe")
    vs = subprocess.check_output(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        text=True).strip()
    redist = sorted(Path(vs).glob("VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT"))
    if not redist:
        raise RuntimeError("The x64 MSVC redistributable files are missing.")
    for file in redist[-1].glob("*.dll"):
        shutil.copy2(file, native)


def deploy_plugins(native, qt, pattern):
    for folder in ("imageformats", "iconengines"):
        dest = native / "plugins" / folder
        dest.mkdir(parents=True, exist_ok=True)
        for source in (qt / "plugins" / folder).glob(pattern):
            shutil.copy2(source, dest)


def linux_runtime(package, runtime, qt):
    native = package / "native"
    for name in ("PeareOpener", "PeareDecoder", "PeareWriter"):
        shutil.copy2(runtime / f"lib{name}.so", native)
    platforms = native / "plugins" / "platforms"
    platforms.mkdir(parents=True)
    shutil.copy2(qt / "plugins" / "platforms" / "libqoffscreen.so", platforms)
    deploy_plugins(native, qt, "*.so")
    binaries = [*native.glob("*.so"), *(native / "plugins").rglob("*.so")]
    env = {**os.environ, "LD_LIBRARY_PATH": str(qt / "lib")}
    for binary in binaries:
        dependencies = subprocess.check_output(["ldd", binary], env=env, text=True)
        for line in dependencies.splitlines():
            left, arrow, right = line.partition("=>")
            path = Path(right.strip().split(" ", 1)[0])
            if arrow and path.is_absolute() and path.resolve().is_relative_to(qt):
                name = left.strip()
                shutil.copy2(path, native / name)
    launcher = package / "peare-mcp"
    launcher.write_text(
        '#!/bin/sh\nset -eu\nroot=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
        'export LD_LIBRARY_PATH="$root/native${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
        'exec "$root/PeareMcp" "$@"\n', encoding="utf-8")
    launcher.chmod(0o755)


def metadata(package):
    for name in ("LICENSE", "COPYRIGHT.md", "THIRDPARTY.md"):
        shutil.copy2(ROOT / name, package)
    shutil.copy2(ROOT / "docs" / "agents" / "AI_AGENT_USAGE.md", package / "USAGE.md")
    python_license = next((p for p in (Path(sys.base_prefix) / "LICENSE.txt",
                                      Path(sys.base_prefix) / "LICENSE") if p.is_file()), None)
    if python_license:
        shutil.copy2(python_license, package / "PYTHON-LICENSE.txt")
    installer = distribution("pyinstaller")
    license_file = next(path for path in installer.files if path.name == "COPYING.txt")
    shutil.copy2(installer.locate_file(license_file), package / "PYINSTALLER-LICENSE.txt")
    (package / "SOURCE.txt").write_text(
        f"Peare source: {REPO}\n"
        "The release commit contains the package build script.\n"
        "PyInstaller bootloader source: https://github.com/pyinstaller/pyinstaller\n"
        "Python source: https://www.python.org/downloads/source/\n",
        encoding="utf-8")


def archive(package, output):
    if WINDOWS:
        zip_path = output / "peare-mcp-windows-x64.zip"
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zip_file:
            for file in sorted(package.rglob("*")):
                if file.is_file():
                    zip_file.write(file, file.relative_to(package))
        manifest = {
            "manifest_version": "0.3", "name": "peare", "version": "1.0.0",
            "display_name": "Peare",
            "description": "Browse, extract, decode and update local archives and disk images.",
            "author": {"name": "Peare contributors", "url": REPO},
            "repository": {"type": "git", "url": REPO},
            "homepage": REPO, "license": "AGPL-3.0",
            "compatibility": {"platforms": ["win32"]},
            "server": {"type": "binary", "entry_point": "PeareMcp.exe",
                       "mcp_config": {"command": "${__dirname}/PeareMcp.exe", "args": []}},
        }
        desktop = output / "peare-mcp-windows-x64.mcpb"
        shutil.copyfile(zip_path, desktop)
        with zipfile.ZipFile(desktop, "a", zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
    else:
        with tarfile.open(output / "peare-mcp-linux-x64.tar.gz", "w:gz") as bundle:
            for file in sorted(package.iterdir()):
                bundle.add(file, arcname=file.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--qt-root", required=True, type=Path)
    parser.add_argument("--offscreen", type=Path, help="Directory containing qoffscreen.dll on Windows")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if WINDOWS and args.offscreen is None:
        parser.error("--offscreen is required on Windows")
    args.output.mkdir(parents=True, exist_ok=True)
    tmp = ROOT / "tmp"
    tmp.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="peare-mcp-package-", dir=tmp) as directory:
        work = Path(directory)
        command = [
            sys.executable, "-m", "PyInstaller", "--noconfirm", "--onedir", "--name", "PeareMcp",
            "--distpath", str(work / "dist"), "--workpath", str(work / "build"),
            "--specpath", str(work),
        ]
        for header in ("opener/api/peare/peare_types.h", "decoder/api/peare/peare_decoder.h"):
            command += ["--add-data", f"{ROOT / 'src' / header}:."]
        subprocess.run([*command, str(ROOT / "tools" / "peare_mcp.py")], check=True, cwd=ROOT)
        package = work / "dist" / "PeareMcp"
        (package / "native").mkdir()
        if WINDOWS:
            windows_runtime(package, args.runtime, args.qt_root.resolve(), args.offscreen)
        else:
            linux_runtime(package, args.runtime, args.qt_root.resolve())
        metadata(package)
        archive(package, args.output)


if __name__ == "__main__":
    main()
