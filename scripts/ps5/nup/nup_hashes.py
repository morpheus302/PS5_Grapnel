# SPDX-License-Identifier: GPL-3.0-or-later
"""The list scripts/ps5/nup/nup_same.c --hashes reads: every file of a .nup's newest title's romfs as nx2/nup.py extracts it (its
copy_to: codec 1 and frames of themselves unpacked, the game's .zs as they are), a line "<sha256> <size> <path>" each. Made from a
player's dump (compressed or not), it says what the console must read in the builder's flat copy of it; game-derived: it stays on
the PC.

    python3 -I scripts/ps5/nup/nup_hashes.py <file.nup> <out list> [--record]

--record: the records' own SHA-256 too, compared with the extraction's (a line "MISMATCH <path>" on stderr for each that differs)
"""
import argparse
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))), "nx2"))
import nup  # noqa: E402


class Sink:
    def __init__(self):
        self.h, self.n = hashlib.sha256(), 0

    def write(self, b):
        self.h.update(b)
        self.n += len(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nup")
    ap.add_argument("out")
    ap.add_argument("--record", action="store_true")
    a = ap.parse_args()
    p = nup.Nup(a.nup)
    t = nup.pick_title(p, None)
    idx = {id(r): i for i, r in enumerate(p.files)}
    t0, n, total, mismatch, codecs, framed = time.time(), 0, 0, 0, {}, 0
    cache = {}
    with open(a.out + ".part", "w", encoding="utf-8") as f:
        f.write("# %s: title %016X v%d, its romfs as nx2/nup.py extracts it (game-derived: keep it on this PC)\n"
                % (os.path.basename(a.nup), t["id"], t["version"]))
        for rel, r in p.walk(t["romfs"]):
            key = (r[3], r[4], r[5], r[2])
            if key not in cache:
                s = Sink()
                if r[5] == 0:
                    framed += p.is_packed(r, rel)
                p.copy_to(r, s, rel, packed=True if r[5] == 1 else None)
                if s.n != r[2]:
                    raise SystemExit("%s: %d bytes extracted, %d listed" % (rel, s.n, r[2]))
                cache[key] = s.h.digest()
            digest = cache[key]
            codecs[r[5]] = codecs.get(r[5], 0) + 1
            if a.record and digest != p.file_details[idx[id(r)]][0]:
                mismatch += 1
                print("MISMATCH %s" % rel, file=sys.stderr)
            f.write("%s %d %s\n" % (digest.hex(), r[2], rel))
            n += 1
            total += r[2]
    os.replace(a.out + ".part", a.out)
    print("%s: %d files, %.2f GB, codecs %s, %d frames of themselves with codec 0, %d distinct extents%s, %.0f s" % (
        a.out, n, total / 1e9, codecs, framed, len(cache), (", %d unlike their records' SHA-256" % mismatch) if a.record else "",
        time.time() - t0))


if __name__ == "__main__":
    main()
