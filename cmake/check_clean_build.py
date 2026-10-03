#!/usr/bin/env python3
"""Build a tracked-source copy and reject source-tree generated headers."""

import argparse
import shutil
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--work", type=Path, required=True)
parser.add_argument("--include", action="append", default=[],
                    help="explicit prospective source addition (relative path or glob)")
args = parser.parse_args()
source = args.source.resolve()
work = args.work.resolve()
if work == source / "build" or not work.is_relative_to(source / "build"):
    parser.error("--work must be below the source's build/ directory")
if work.exists():
    shutil.rmtree(work)
export = work / "source"
export.mkdir(parents=True)
files = subprocess.check_output(["git", "ls-files", "-z"], cwd=source).decode().split("\0")
for pattern in args.include:
    for path in source.glob(pattern):
        if path.is_file() and path.resolve().is_relative_to(source):
            files.append(str(path.relative_to(source)))
for name in filter(None, files):
    destination = export / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / name, destination)
binary = work / "candidate"
subprocess.run(["cmake", "-S", str(export), "-B", str(binary), "-G", "Ninja",
                "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], check=True)
subprocess.run(["cmake", "--build", str(binary), "--parallel", "8"], check=True)
subprocess.run(["ctest", "--test-dir", str(binary), "--output-on-failure"], check=True)
dependencies = subprocess.check_output(["ninja", "-C", str(binary), "-t", "deps"]).decode()
for header in ("config.h", "osdef.h", "wayland/wlr-data-control-unstable-v1.h",
               "wayland/ext-data-control-v1.h"):
    assert str(export / "src/auto" / header) not in dependencies, header
print("Tracked-source build and generated-header routing passed")
