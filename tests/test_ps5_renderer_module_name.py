"""On the console the Vulkan back end is linked into eboot.bin, and LoadLibraryA finds it by name in a table
(host/posix/ps5_platform.c hos_ps5_module). The name host/rx_backend.c asks for must be one the table knows: after upstream's
rename the host asked for longshot_vk.dll while the table knew hookshot_vk alone, and the merged eboot's first run on the
console drew nothing ("rx: /data/hookshot/longshot_vk.so did not load (): nothing is drawn", 2026-11-16).

Both files are read as text: every name rx_backend.c hands to load_vulkan, and the Vulkan loader's name in gpu/vk/vk_main.cpp,
against the prefixes hos_ps5_module compares. No game, GPU, console or compiler.

    python -I -B tests/test_ps5_renderer_module_name.py
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    backend = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    platform = (ROOT / "host/posix/ps5_platform.c").read_text(encoding="utf-8")
    vk_main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")

    start = platform.index("void *hos_ps5_module(const char *path) {")
    table = platform[start:platform.index("\n}\n", start)]
    known = [(name.lower(), int(n)) for name, n in re.findall(r'strncasecmp\(base, "([^"]+)", (\d+)\)', table)]
    if not known:
        raise SystemExit("FAIL: hos_ps5_module compares no name (the table was rewritten: bring this check up to date)")
    for name, n in known:
        if len(name) != n:
            raise SystemExit("FAIL: hos_ps5_module compares %d characters of %r, which has %d" % (n, name, len(name)))

    asked = set(re.findall(r'load_vulkan\("([^"]+)"\)', backend))
    asked |= set(re.findall(r'const char \*dll = "([^"]+)";', backend))
    if not asked:
        raise SystemExit("FAIL: host/rx_backend.c names no back end to load (rewritten: bring this check up to date)")
    loader = set(re.findall(r'LoadLibraryA\("(vulkan[^"]+)"\)', vk_main))
    if not loader:
        raise SystemExit("FAIL: gpu/vk/vk_main.cpp no longer loads the Vulkan loader by name (bring this check up to date)")

    missing = sorted(a for a in asked | loader if not any(a.lower().startswith(name) for name, _ in known))
    if missing:
        raise SystemExit("FAIL: the console's module table (hos_ps5_module) does not know %s; it knows %s"
                         % (", ".join(missing), ", ".join(name for name, _ in known)))
    print("ok: the console's module table knows %s" % ", ".join(sorted(asked | loader)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
