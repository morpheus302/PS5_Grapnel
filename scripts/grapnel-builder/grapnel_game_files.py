# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel: the game's files checked against a reference, from the .nup's own TOC (no file read): corrupted and missing files.

A .nup's TOC holds the SHA-256 of every file's content (the file as the game has it, whatever the dump compressed: checked on a
player's dump, every one of its 186,587 romfs files, codec 1 included). A dump that read a file wrong (a player's 23 GB dump on
25/11/2026: 9 files with blocks of zeros, the Water Temple's gate texture among them, their records made from the zeros) has
another SHA-256 there than a sound dump. So step 1 compares the TOC with a reference made from a sound dump of the same version
(the port's own, which plays from start to end), in a few tenths of a second:
  - a file whose SHA-256 differs from the reference's: corrupted (the game gets other bytes than its own);
  - a file of the reference the dump does not have: missing;
  - a file of the dump the reference does not have: not said (other dumpers keep a few more files: 4 small settings files in
    that player's dump; nothing wrong with them).
It cannot see a file whose bytes were damaged after the dump wrote its TOC (a copy that went wrong): the conversion of a
compressed dump reads every file and sees those too (grapnel_flat.py), and a game .zs that does not start as zstd is named
there as well.

The reference (data/game_files_v131072.ref.xz): no content of the game, its files' paths and the first 8 bytes of their
SHA-256 (2^-64 of missing a damaged file: enough, nothing here is an attack), sorted by path, xz-compressed (1.9 MB):
    a first line of JSON (format, title, count, hash bytes, what it was made from), then the paths (exefs/..., romfs/...), one a
    line, then the hashes, 8 bytes each in the paths' order.

    python3 -I scripts/grapnel-builder/grapnel_game_files.py make <sound.nup> [--out F] [--verify]   (the reference: once a version)
    python3 -I scripts/grapnel-builder/grapnel_game_files.py check <file.nup>                        (a dump against it)
"""
import hashlib
import json
import lzma
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NX2 = os.path.join(os.path.dirname(os.path.dirname(HERE)), "nx2")
if NX2 not in sys.path:
    sys.path.insert(0, NX2)
import nup  # noqa: E402

REFERENCE = os.path.join(HERE, "data", "game_files_v131072.ref.xz")
FORMAT = "grapnel-game-files 1"
HASH_BYTES = 8
SHOWN = 12   # (the files a window or a log names, at most; the count says the rest)

# what a file is, from its folder and name: the first rule that matches (a key the window says in its language)
KINDS = (("exefs/", None, "program"), (None, ".bntx.zs", "texture"), ("romfs/Tex/", None, "texture"),
         (None, ".bfres.zs", "model"), ("romfs/Model/", None, "model"), ("romfs/TerrainArc/", None, "terrain"),
         (None, ".extm", "terrain"), (None, ".hght", "terrain"), (None, ".mate", "terrain"), ("romfs/Voice/", None, "voice"),
         (None, ".webm", "movie"), ("romfs/Sound/", None, "sound"), (None, ".bwav", "sound"), (None, ".bars.zs", "sound"),
         ("romfs/Shader/", None, "shader"), (None, ".bfsha.zs", "shader"), ("romfs/Font", None, "font"), ("romfs/UI/", None, "ui"),
         ("romfs/Layout", None, "ui"), ("romfs/Mals/", None, "text"), (None, ".msbt", "text"), (None, ".ainb", "ai"),
         ("romfs/AI/", None, "ai"), (None, ".bphcl", "physics"), (None, ".bphsc.zs", "physics"), (None, ".bphsh.zs", "physics"),
         (None, ".bphnm.zs", "physics"), ("romfs/Effect/", None, "effect"), ("romfs/Event/", None, "event"),
         (None, ".bgyml", "setting"), (None, ".byml.zs", "setting"), ("romfs/gsys/", None, "setting"), ("romfs/Pack/", None, "pack"))
KIND_EN = {"program": "the game's program", "texture": "texture", "model": "3D model", "terrain": "terrain", "voice": "voice",
           "movie": "movie", "sound": "sound", "shader": "shader", "font": "font", "ui": "interface", "text": "text",
           "ai": "behaviour", "physics": "physics", "effect": "effect", "event": "event", "setting": "setting",
           "pack": "objects' pack", "other": "game file"}


def kind_of(path):
    """What a file of the game is (a key of KIND_EN), from its path ("romfs/Tex/x.bntx.zs": texture)"""
    for start, end, kind in KINDS:
        if (start is None or path.startswith(start)) and (end is None or path.endswith(end)):
            return kind
    return "other"


def files_of(p, t):
    """{path: (size, SHA-256 from the TOC, record)} of the title's exefs and romfs, the paths as the reference has them"""
    idx = {id(r): i for i, r in enumerate(p.files)}
    out = {}
    for part in ("exefs", "romfs"):
        if t[part] == 0xffffffff:
            continue
        for rel, r in p.walk(t[part]):
            out[part + "/" + rel] = (r[2], p.file_details[idx[id(r)]][0], r)
    return out


class _HashSink:
    def __init__(self):
        self.h = hashlib.sha256()

    def write(self, b):
        self.h.update(b)


def make(src, out, verify=False, progress=None):
    """The reference from a sound dump (its newest title). verify: every file read and its SHA-256 checked against its record
    first (the TOC's hashes are what the reference keeps): a dump that is not what its TOC says is refused"""
    p = nup.Nup(src)
    try:
        t = nup.pick_title(p, None)
        files = files_of(p, t)
        if verify:
            seen = {}
            for i, (path, (size, digest, r)) in enumerate(sorted(files.items())):
                key = (r[3], r[4], r[5], r[2])
                if key not in seen:
                    s = _HashSink()
                    p.copy_to(r, s, path.split("/", 1)[1], packed=True if r[5] == 1 else None)
                    seen[key] = s.h.digest()
                if seen[key] != digest:
                    raise SystemExit("%s: its content is not what its record's SHA-256 says: not a sound dump" % path)
                if progress and i % 5000 == 0:
                    progress(i, len(files))
        rows = sorted(files.items())
        head = {"format": FORMAT, "title": "%016X v%d" % (t["id"], t["version"]), "files": len(rows), "hash_bytes": HASH_BYTES,
                "made_from": os.path.basename(src), "verified": bool(verify)}
        blob = (json.dumps(head) + "\n" + "".join(path + "\n" for path, _ in rows)).encode("utf-8") + \
            b"".join(v[1][:HASH_BYTES] for _, v in rows)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out + ".part", "wb") as f:
            f.write(lzma.compress(blob, preset=9 | lzma.PRESET_EXTREME))
        os.replace(out + ".part", out)
        return head
    finally:
        p.f.close()


_loaded = {}


def reference_path():
    """The reference step 1 uses: data/'s, or GRAPNEL_GAME_FILES_REFERENCE's (the tests' own, made from their small packages)"""
    return os.environ.get("GRAPNEL_GAME_FILES_REFERENCE") or REFERENCE


def load(path=None):
    """(head, {path: first HASH_BYTES of its SHA-256}) of a reference, or (None, None) when there is none"""
    path = path or reference_path()
    if path in _loaded:
        return _loaded[path]
    try:
        with open(path, "rb") as f:
            blob = lzma.decompress(f.read())
    except (OSError, lzma.LZMAError):
        return None, None
    first = blob.index(b"\n")
    head = json.loads(blob[:first])
    n, hb = head["files"], head["hash_bytes"]
    hashes = blob[len(blob) - n * hb:]
    paths = blob[first + 1:len(blob) - n * hb].decode("utf-8").split("\n")[:-1]
    if len(paths) != n or head.get("format") != FORMAT:
        return None, None
    ref = {paths[i]: hashes[i * hb:(i + 1) * hb] for i in range(n)}
    _loaded[path] = (head, ref)
    return head, ref


def check(p, t, reference=None):
    """The title's files against the reference, from the TOC alone: {"reference": its title (None: no reference, nothing said),
    "checked": files compared, "corrupted": [paths whose SHA-256 differs], "missing": [paths of the reference not in the dump],
    "unknown": files of the dump the reference does not have (not a fault)}"""
    head, ref = load(reference)
    out = {"reference": None, "checked": 0, "corrupted": [], "missing": [], "unknown": 0}
    if ref is None:
        return out
    if head["title"].split(" v")[1] != str(t["version"]):   # (a reference of another version says nothing of this one)
        return out
    hb = head["hash_bytes"]
    files = files_of(p, t)
    out["reference"] = head["title"]
    for path, (size, digest, r) in files.items():
        want = ref.get(path)
        if want is None:
            out["unknown"] += 1
            continue
        out["checked"] += 1
        if digest[:hb] != want:
            out["corrupted"].append(path)
    out["missing"] = sorted(path for path in ref if path not in files)
    out["corrupted"].sort()
    return out


def lines(found, limit=SHOWN):
    """The files found, a line each ("  romfs/Tex/x.bntx.zs (texture)"), at most limit, then how many more"""
    every = [("corrupted", f) for f in found.get("corrupted", [])] + [("missing", f) for f in found.get("missing", [])]
    out = ["  %s: %s (%s)" % (what, f, KIND_EN[kind_of(f)]) for what, f in every[:limit]]
    if len(every) > limit:
        out.append("  ... and %d more" % (len(every) - limit))
    return out


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="what", required=True)
    m = sub.add_parser("make")
    m.add_argument("nup")
    m.add_argument("--out", default=REFERENCE)
    m.add_argument("--verify", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("nup")
    c.add_argument("--reference")
    a = ap.parse_args()
    if a.what == "make":
        head = make(a.nup, a.out, a.verify, progress=lambda i, n: print("  %d/%d" % (i, n), flush=True))
        print("%s: %s, %d files, %d bytes" % (a.out, head["title"], head["files"], os.path.getsize(a.out)))
        return 0
    p = nup.Nup(a.nup)
    try:
        found = check(p, nup.pick_title(p, None), a.reference)
    finally:
        p.f.close()
    print("reference %s: %d files checked, %d corrupted, %d missing, %d not in the reference" % (
        found["reference"], found["checked"], len(found["corrupted"]), len(found["missing"]), found["unknown"]))
    print("\n".join(lines(found, 1000)))
    return 1 if found["corrupted"] or found["missing"] else 0


if __name__ == "__main__":
    sys.exit(main())
