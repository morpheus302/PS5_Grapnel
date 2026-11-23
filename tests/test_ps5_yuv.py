"""Build and run tests/ps5_yuv_test.c: host/posix/ps5_yuv.h (the console's 4:2:0 to RGBA) against the plain loop it replaced.

Twice: with AVX2 (the console's path; on an x86-64 PC only) and without (the plain C alone). The pictures are authored
random bytes: no game, no console, no decoder. --bench also times a 1920x1080 picture each way.
"""
import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--bench", action="store_true")
    args = parser.parse_args()
    builds = [("plain", [])]
    if platform.machine().lower() in ("x86_64", "amd64"):
        builds.append(("avx2", ["-mavx2"]))
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="ps5-yuv-") as temporary:
        for name, extra in builds:
            executable = Path(temporary) / (name + ".exe")
            command = [args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"] + extra + [
                str(ROOT / "tests/ps5_yuv_test.c"), "-o", str(executable)]
            for command in (command, [str(executable)] + (["bench"] if args.bench else [])):
                result = subprocess.run(command, creationflags=flags, capture_output=True, text=True,
                                        encoding="utf-8", errors="replace", timeout=120)
                print(result.stdout, end="")
                print(result.stderr, end="", file=sys.stderr)
                if result.returncode:
                    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
