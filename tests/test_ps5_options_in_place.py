"""On the console the Insert menu's save (gpu/vk/vk_menu.cpp save) writes hookshot_options.ini in place, as it did before
upstream's 1576a274 (#550) was merged: no <name>.new beside it, nothing moved over it, and none of realpath, access and fchmod,
which the title has never run on /data. Everywhere else the save is upstream's (written beside the file and moved over it:
tests/test_menu_save_atomic.py runs that one).

The real save() is cut out of the source and passed through the preprocessor twice, with and without __PROSPERO__; what is
left is read as text. No game, GPU, console or window.

    python -I -B tests/test_ps5_options_in_place.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "the preprocessor there defines _WIN32: the two POSIX branches are what is compared"}

ROOT = Path(__file__).resolve().parents[1]
MOVED = ('".new"', "rename(", "realpath(", "access(", "fchmod(")   # (what the save beside the file needs)
IN_PLACE = 'fopen(path.c_str(), "w")'


def preprocessed(cxx, text, defines):
    r = subprocess.run([cxx, "-E", "-P", "-x", "c++", "-"] + ["-D" + d for d in defines], input=text, capture_output=True,
                       text=True, encoding="utf-8", errors="replace", timeout=60)
    if r.returncode:
        raise SystemExit("the preprocessor failed (%s): %s" % (" ".join(defines) or "no define", r.stderr))
    return r.stdout


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="c++")
    a = p.parse_args()
    save = block((ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8"), "void save() {")
    console = preprocessed(a.cxx, save, ["__PROSPERO__"])
    elsewhere = preprocessed(a.cxx, save, [])
    bad = []
    for what in MOVED:
        if what in console:
            bad.append("on the console, save() still has %s" % what)
        if what not in elsewhere:
            bad.append("off the console, save() lost %s (upstream's save beside the file)" % what)
    for name, text in (("on the console", console), ("off the console", elsewhere)):
        if text.count(IN_PLACE) != 1:
            bad.append("%s, save() opens the file in place %d times, not once" % (name, text.count(IN_PLACE)))
    if "in_place = true" not in console:
        bad.append("on the console, save() does not say it writes in place")
    if bad:
        raise SystemExit("\n".join(bad))
    print("save(): in place on the console (%d lines), beside the file and moved elsewhere (%d lines)"
          % (console.count("\n"), elsewhere.count("\n")))


if __name__ == "__main__":
    main()
