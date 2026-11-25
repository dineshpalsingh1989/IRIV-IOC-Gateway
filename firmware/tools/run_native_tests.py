"""Builds and runs the libraries' native (PC) tests.

Usage:  python tools/run_native_tests.py
Needs a host C++ compiler: g++, clang++, `zig c++`, or set CXX.
These tests check the portable logic only; they do not prove hardware behaviour.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")
SUITES = [
    ("IRIVModbus", ["tests/core_tests.cpp", "src/IRIVModbusCore.cpp"]),
    ("IRIVIOCIo", ["tests/logic_tests.cpp"]),
]


def compiler():
    if os.environ.get("CXX"):
        return os.environ["CXX"].split()
    for name in ("g++", "clang++"):
        if shutil.which(name):
            return [name]
    if shutil.which("zig"):
        return ["zig", "c++"]
    sys.exit("No C++ compiler found: install g++/clang++/zig or set CXX")


def main():
    cxx = compiler()
    failed = 0
    with tempfile.TemporaryDirectory() as out:
        for lib, sources in SUITES:
            exe = os.path.join(out, lib + (".exe" if os.name == "nt" else ""))
            files = [os.path.join(ROOT, lib, s) for s in sources]
            build = subprocess.run(cxx + ["-std=c++11", "-Wall", "-Wextra", "-O1", "-o", exe] + files)
            if build.returncode:
                print(f"{lib}: build failed")
                failed += 1
                continue
            failed += subprocess.run([exe]).returncode != 0
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
