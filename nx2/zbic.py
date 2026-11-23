"""ZBIC decompression (Switch firmware 22.0.0+'s zstd variant) through a program of its own, zbic_unpack, built on first use.

    import zbic; plain = zbic.decompress(data)

(grapnel) The ZBIC parts of third_party/zbic are Atmosphere-NX's, GPL-2.0; these tools are GPL-3.0. They were loaded into this
Python with ctypes (zbic.so), which makes one program of both. They are now compiled into nx2/zbic_unpack/zbic_unpack (the C
in nx2/zbic_unpack/zbic_unpack.c with third_party/zbic/zstd.c), a separate GPL-2.0 program this module runs with the frame on
its stdin and reads the bytes back from its stdout: two programs that exchange data, with a clean licence boundary.
The segment hashes nx2/unpack.py checks after each call still say whether the bytes are right.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit  # noqa: F401  (a64-kit on sys.path)
import toolchain

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), "third_party", "zbic")
PROGRAM_SRC = os.path.join(HERE, "zbic_unpack", "zbic_unpack.c")
EXE = os.path.join(HERE, "zbic_unpack", "zbic_unpack.exe" if os.name == "nt" else "zbic_unpack")


def build():
    """The program, compiled once by the PC's clang (the one that came with the tools is used as it is); its path"""
    if os.path.exists(EXE):
        return EXE
    cmd = [toolchain.need(toolchain.find("clang", host=True)), *toolchain.native_target(host=True), *toolchain.native_link(host=True),
           "-O2", "-DZSTD_ZBIC_SUPPORT=1", "-I", SRC, "-o", EXE + ".part", PROGRAM_SRC, os.path.join(SRC, "zstd.c")]
    print("building", os.path.basename(EXE), file=sys.stderr)
    subprocess.run(cmd, check=True)
    os.replace(EXE + ".part", EXE)
    return EXE


def _run(args, data):
    r = subprocess.run([build()] + args, input=data, capture_output=True)
    if r.returncode:
        raise ValueError("ZBIC: %s" % (r.stderr.decode("utf-8", "replace").strip() or "zbic_unpack exited %d" % r.returncode))
    return r.stdout


def selftest():
    """zbic_unpack --selftest: its zstd compresses and decompresses a pattern; the program's answer line"""
    return _run(["--selftest"], b"").decode().strip()


def is_zbic(data):
    return data[:4] == b"ZBIC"


def decompress(data, size=None):
    if size is None:
        size = int(_run(["--content-size"], data).decode().strip())
    out = _run([str(size)], data)
    if len(out) != size:
        raise ValueError("ZBIC: decompression failed (%d of %d bytes)" % (len(out), size))
    return out
