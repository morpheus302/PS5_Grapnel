"""The flat copy of a .nup whose game files are stored compressed (scripts/grapnel-builder/grapnel_flat.py), with no game.

Two players' dumps on 25/11/2026 kept a fifth of the game's files as zstd frames (codec 1: 40,588 files, ZsDic.pack.zs among
them, a .zs of the game with an outer frame over it). The console reads the game file in place (host/nup_fs.c) and opens no
compressed file: the game stopped as Play was pressed. The builder now makes, once, a copy with every file as itself, and that
is what the console is given. Small packages made here, with what dumpers write:
  - codec 1 over plain files and over .zs files of the game (the outer frame removed, the game's .zs kept as it is);
  - a newer dumper's package: every file a zstd frame of itself with codec 0 (stored size the frame's, or the file's), tiny and
    empty files, a .zs of the game left as it is among them, a .zs whose own frame states its own size (made with a dictionary);
  - two titles (the base game and its update): the copy has the newest alone; files that share their bytes are kept once.
Checked: every file of the copy is what nx2/nup.py extracts from the source (exefs and romfs), the copy is flat and whole (one
title, codec 0, stored size = size, the TOC's SHA-256 in its header, nx2/nup.py's strict reading and --verify pass), a normal
.nup needs no copy, a stopped conversion goes on (and ends as a fresh one does), a copy made before is taken again, a changed
source is copied again, too little disk space stops before anything is written, a game.nup not ours is never overwritten, a
damaged frame stops with its name, a file the dump holds otherwise than its record says is copied as extracted and named.
Then host/nup_fs.c, as the console has it, compiled on this PC (scripts/ps5/nup/nup_same.c): it refuses or cannot open the
compressed sources' files, and reads the copies with nothing compressed and nothing bad, every file the same as a flat reference.
Without the change (0.2.3/0.2.4: no grapnel_flat.py, check_nup refusing the package) this fails.

    python -I -B tests/test_grapnel_flat.py [--cc gcc]     (needs zstandard: hookshot-ps5/.venv/bin/python3 here)
"""
import argparse
import contextlib
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import zstandard

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_MODULES = ["zstandard"]
RUN_ALL_SKIP = {"windows": "compiles host/nup_fs.c with pthreads: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "grapnel-builder"))
sys.path.insert(0, str(ROOT / "nx2"))
import grapnel_flat as flat  # noqa: E402
import nup  # noqa: E402

BASE_ID, UPDATE_ID, VERSION = 0x0400486024DEC000, 0x0400486024DEC800, 131072


def align(n):
    return (n + 15) & ~15


def make_nup(path, titles):
    """A NUP package as dumpers write it (nx2/nup.py's layout): titles = [dict(id, version, meta, files={path under the title's
    root ("exefs/x", "romfs/a/b"): (stored bytes, size, codec[, stored size field]))}]. Files with the same stored bytes share one
    extent, as a dump's do"""
    dirs, files, kids_of, names = [], [], [], bytearray()
    name_at = {}

    def name(s):
        b = s.encode()
        if b not in name_at:
            name_at[b] = len(names)
            names.extend(b)
        return name_at[b], len(b)

    def new_dir(n):
        dirs.append(name(n))
        kids_of.append([])
        return len(dirs) - 1
    roots = []
    for t in titles:
        root = new_dir(str(t["version"]))
        sub = {"": root}
        for part in ("exefs", "romfs"):
            d = new_dir(part)
            kids_of[root].append(("d", d))
            sub[part] = d
        for rel, spec in t["files"].items():
            parts = rel.split("/")
            at = ""
            for i, p in enumerate(parts[:-1]):
                key = "/".join(parts[:i + 1])
                if key not in sub:
                    sub[key] = new_dir(p)
                    kids_of[sub[at]].append(("d", sub[key]))
                at = key
            files.append((name(parts[-1]), spec))
            kids_of[sub[at]].append(("f", len(files) - 1))
        roots.append((root, sub["exefs"], sub["romfs"]))
    nt, nd, nf = len(titles), len(dirs), len(files)
    order_kids, firsts = [], []
    for d in range(nd):
        firsts.append(len(order_kids))
        order_kids += [k if kind == "d" else nd + k for kind, k in kids_of[d]]
    soff = 0x30 + 0x40 * nt + 24 * nd + 80 * nf + 4 * len(order_kids)
    toc_size = align(soff + len(names))
    at = align(0x80 + toc_size)
    metas = []
    for t in titles:
        metas.append(at)
        at = align(at + len(t["meta"]))
    data_at = at
    blobs, offsets = bytearray(), {}
    recs = bytearray()
    for (no, nl), spec in files:
        stored, size, codec = spec[:3]
        if size == 0:
            off, sfield = 0, 0
        else:
            if stored not in offsets:
                offsets[stored] = data_at + len(blobs)
                blobs += stored
                blobs += bytes(align(len(blobs)) - len(blobs))
            off, sfield = offsets[stored], (spec[3] if len(spec) > 3 else len(stored))
        digest = spec[4] if len(spec) > 4 else hashlib.sha256(logical(stored, size, codec)).digest()
        recs += struct.pack("<IIQQQ", no, nl, size, off, sfield) + digest + bytes([codec, 0]) + bytes(14)
    toc = bytearray(b"TOC\x01" + struct.pack("<IQQQQ", nt, nd, nf, soff, len(names)) + bytes(8))
    for i, t in enumerate(titles):
        r, e, ro = roots[i]
        toc += struct.pack("<QIIIIQQQ", t["id"], r, e, ro, t["version"], i, metas[i], len(t["meta"])) + bytes(16)
    for d in range(nd):
        toc += struct.pack("<4IQ", dirs[d][0], dirs[d][1], firsts[d], len(kids_of[d]), 0)
    toc += recs + struct.pack("<%dI" % len(order_kids), *order_kids) + names
    toc += bytes(toc_size - len(toc))
    total = data_at + len(blobs)
    out = bytearray(data_at)
    out[0:4] = b"NUP\x01"
    out[4:8] = b"\x80\x00\x01\x00"
    struct.pack_into("<QQ", out, 8, 0x80, toc_size)
    out[0x18:0x38] = hashlib.sha256(toc).digest()
    struct.pack_into("<QQQ", out, 0x38, data_at, total, nt)
    out[0x50:0x60] = bytes(range(16))
    out[0x80:0x80 + toc_size] = toc
    for i, t in enumerate(titles):
        out[metas[i]:metas[i] + len(t["meta"])] = t["meta"]
    with open(path, "wb") as f:
        f.write(out + blobs)


# the files' contents: what the game has ("logical"); a test spec stores them as the dumper would
_LOGICAL = {}


def logical(stored, size, codec):
    return _LOGICAL.get((bytes(stored), size, codec), bytes(stored))


def as_codec1(content, level=3):
    frame = zstandard.ZstdCompressor(level=level, write_checksum=True).compress(content)
    _LOGICAL[(frame, len(content), 1)] = content
    return (frame, len(content), 1)


def as_frame(content, stored_field="frame", sized=True):
    """codec 0, the file a zstd frame of itself (a newer dumper's): its record's stored size the frame's, or the file's"""
    if sized:
        frame = zstandard.ZstdCompressor(level=3, write_content_size=True).compress(content)
    else:   # (a streamed frame, no content size in its header)
        c = zstandard.ZstdCompressor(level=3, write_content_size=False).compressobj()
        frame = c.compress(content) + c.flush()
    _LOGICAL[(frame, len(content), 0)] = content
    return (frame, len(content), 0) + ((len(content),) if stored_field == "size" else ())


def raw(content):
    return (content, len(content), 0)


def rnd(n, seed):
    out, x = bytearray(), seed
    while len(out) < n:
        x = (x * 6364136223846793005 + 1442695040888963407) & (2 ** 64 - 1)
        out += x.to_bytes(8, "little")
    return bytes(out[:n])


def text(n, seed):
    words = [b"link", b"zelda", b"ocarina", b"hyrule", b"temple", b"water", b"fire", b"forest", b"shadow", b"spirit"]
    x, out = seed, bytearray()
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7fffffff
        out += words[x % len(words)] + b" "
    return bytes(out[:n])


def game_zs(content, dict_id=None):
    """a .zs file of the game: zstd in the game itself, its frame's content larger than the file"""
    return zstandard.ZstdCompressor(level=9, write_content_size=True).compress(content)


def odd_zs(size):
    """a .zs of the game whose own frame states its own size (three of the game's do), made with a dictionary (id 7): zstd with no
    dictionary refuses it, so it is the game's own, not a frame of itself"""
    head = b"\x28\xb5\x2f\xfd" + bytes([0x80 | 0x01]) + bytes([0x58]) + struct.pack("<B", 7) + struct.pack("<I", size)
    return head + rnd(size - len(head), 99)


GAME = {   # (one tree, the files' contents as the game has them)
    "exefs/main": b"NSO0" + rnd(70000, 1),
    "exefs/main.npdm": b"META" + rnd(300, 2),
    "romfs/Pack/ZsDic.pack.zs": game_zs(text(300000, 3)),
    "romfs/Pack/Actor/Link.pack.zs": game_zs(text(200000, 4)),
    "romfs/Sound/Resource/Stream/Bgm.bwav": b"BWAV" + text(900000, 5),
    "romfs/Sound/Resource/Stream/Voice.bwav": b"BWAV" + text(1300, 6),
    "romfs/Terrain/A/0.extm": rnd(120000, 7),
    "romfs/Terrain/A/1.extm": rnd(120000, 7),          # (the same bytes as 0.extm: one extent)
    "romfs/AI/x.ainb": text(5000, 8),
    "romfs/Tiny/one.bin": b"Z",
    "romfs/Tiny/five.bin": b"12345",
    "romfs/Tiny/empty.bin": b"",
    "romfs/Font/Odd.bfotf.zs": odd_zs(4096),
    "romfs/Movie/Title.webm": rnd(3 << 20, 9),
}
for _i in range(70):   # (enough plain files for the console's sample of 64)
    GAME["romfs/Misc/%02d/f%03d.dat" % (_i // 10, _i)] = text(600 + 37 * _i, 100 + _i)


def tree(kind):
    """The game's files as a dumper of that kind stores them"""
    files = {}
    for rel, content in GAME.items():
        if kind == "raw" or not content:
            files[rel] = raw(content)
        elif kind == "codec1":   # (what compresses, codec 1; the rest raw: as those dumps, 40,588 of 186,587)
            frame = zstandard.ZstdCompressor(level=3).compress(content)
            files[rel] = as_codec1(content) if len(frame) < len(content) else raw(content)
        elif kind in ("framed", "framed-size"):   # (every file a frame of itself)
            files[rel] = as_frame(content, "size" if kind == "framed-size" else "frame", sized=not rel.endswith(".ainb"))
    if kind == "codec1":
        files["romfs/Pack/ZsDic.pack.zs"] = as_codec1(GAME["romfs/Pack/ZsDic.pack.zs"])   # (the player's log: this one, codec 1)
    if kind in ("framed", "framed-size"):
        files["romfs/Pack/Actor/Link.pack.zs"] = raw(GAME["romfs/Pack/Actor/Link.pack.zs"])   # (left as it is: a game .zs)
        files["romfs/Font/Odd.bfotf.zs"] = raw(GAME["romfs/Font/Odd.bfotf.zs"])
    return files


def package(path, kind, titles="update-under-base"):
    files = tree(kind)
    if titles == "update-under-base":   # (the player's: one title, the update under the base game's id)
        t = [dict(id=BASE_ID, version=VERSION, meta=b"META" + rnd(1832, 50), files=files)]
    else:   # (base game and update: the base's files partly the update's)
        base = {k: v for k, v in tree("raw").items() if k != "romfs/Movie/Title.webm"}
        base["romfs/Old/only_in_base.bin"] = raw(rnd(50000, 77))
        t = [dict(id=BASE_ID, version=65536, meta=b"META" + rnd(1928, 51), files=base),
             dict(id=UPDATE_ID, version=VERSION, meta=b"META" + rnd(1832, 52), files=files)]
    make_nup(path, t)


def extract_all(path):
    """{rel: bytes} of the newest title's exefs and romfs, as nx2/nup.py extracts them"""
    p = nup.Nup(path)
    try:
        t = nup.pick_title(p, None)
        out = {}
        for part in ("exefs", "romfs"):
            for rel, r in p.walk(t[part]):
                b = io.BytesIO()
                p.copy_to(r, b, rel)
                out[part + "/" + rel] = b.getvalue()
        return out, t
    finally:
        p.f.close()


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def flat_ok(copy, source):
    """The copy: what nx2/nup.py extracts from the source, file for file; flat, one title, its TOC's hash right; nup.py's
    strict reading and --verify happy; no copy needed of it"""
    want, ts = extract_all(source)
    got, tc = extract_all(copy)
    check(set(got) == set(want), "the copy's files are not the source's: %s" % sorted(set(got) ^ set(want))[:5])
    for rel in want:
        check(got[rel] == want[rel], "%s differs from what nx2/nup.py extracts" % rel)
    check(got.get("romfs/Pack/ZsDic.pack.zs", b"")[:4] == b"\x28\xb5\x2f\xfd" and got["romfs/Pack/ZsDic.pack.zs"] ==
          GAME["romfs/Pack/ZsDic.pack.zs"], "ZsDic.pack.zs is not the game's .zs (its outer layer kept, or the game's own unpacked)")
    check(got["romfs/Pack/Actor/Link.pack.zs"] == GAME["romfs/Pack/Actor/Link.pack.zs"], "a game .zs was changed")
    check(got["romfs/Font/Odd.bfotf.zs"] == GAME["romfs/Font/Odd.bfotf.zs"], "the .zs with a dictionary was changed")
    for rel, content in GAME.items():
        check(got[rel] == content, "%s is not the game's file" % rel)
    p = nup.Nup(copy, strict=True)   # (the TOC's SHA-256 against the header, the tree, no orphans)
    try:
        check(len(p.titles) == 1 and (p.titles[0]["id"], p.titles[0]["version"]) == (ts["id"], ts["version"]),
              "the copy's title is not the source's newest: %s" % p.titles)
        for r in p.files:
            check(r[5] == 0 and r[4] == r[2], "a record of the copy is not flat: %s" % (r,))
        with contextlib.redirect_stdout(io.StringIO()):
            bad = nup.verify(p)["bad"]
        check(bad == 0, "nx2/nup.py --verify finds the copy bad")
    finally:
        p.f.close()
    check(not flat.needs_flat(flat.inspect(copy)), "the copy would be copied again")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise AssertionError("%s failed:\n%s%s" % (cmd[0], r.stdout[-3000:], r.stderr[-3000:]))
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="gcc")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="grapnel-flat-") as tmp:
        tmp = Path(tmp)
        # ---- what needs a copy ----
        ref = tmp / "raw.nup"
        package(ref, "raw")
        info = flat.inspect(str(ref))
        check(not flat.needs_flat(info) and info["coded"] == 0 and info["framed"] == 0, "a normal .nup asks for a copy: %s" % info)
        sources = {}
        for kind in ("codec1", "framed", "framed-size"):
            src = tmp / ("%s.nup" % kind)
            package(src, kind)
            sources[kind] = src
            info = flat.inspect(str(src))
            check(flat.needs_flat(info), "%s: no copy asked for (%s)" % (kind, info))
            if kind == "codec1":
                check(info["coded"] >= 70 and info["framed"] == 0, "codec 1 counted wrong: %s" % info)
            else:
                check(info["coded"] == 0 and info["framed"] > 0, "frames of themselves not found: %s" % info)
        # ---- the copies ----
        copies = {}
        for kind, src in sources.items():
            folder = tmp / ("out-" + kind)
            said = []
            s = flat.convert(str(src), str(folder), info=said.append)
            copy = Path(s["path"])
            check(copy == folder / "game.nup" and not (folder / "game.nup.part").exists(), "the copy is not where it should be")
            flat_ok(str(copy), str(src))
            check(s["unpacked_codec1"] + s["unpacked_frames"] > 0 and s["verified"] and not s["reused"], "summary: %s" % s)
            if kind == "codec1":
                check(s["unpacked_codec1"] >= 70 and s["unpacked_frames"] == 0, "codec 1 unpacked: %s" % s)
            else:
                check(s["unpacked_frames"] >= 70 and s["unpacked_codec1"] == 0, "frames unpacked: %s" % s)
            copies[kind] = copy
        # the same copy whatever the stored size field of the frames says
        check(copies["framed"].read_bytes() == copies["framed-size"].read_bytes(), "the two framed dumps' copies differ")
        # two titles: the newest alone, files that share their bytes once
        two = tmp / "two.nup"
        package(two, "codec1", titles="base-and-update")
        s = flat.convert(str(two), str(tmp / "out-two"))
        flat_ok(s["path"], str(two))
        p = nup.Nup(s["path"])
        exts = {(r[3], r[4]) for r in p.files if r[2]}
        check(len(exts) == len(p.files) - 2, "files with the same bytes are not kept once (%d extents, %d files)" % (len(exts), len(p.files)))
        check(not any(rel.endswith("only_in_base.bin") for rel, _ in p.walk(p.titles[0]["romfs"])), "the base game's files are in the copy")
        p.f.close()
        # ---- taken again, made again, gone on with ----
        src, folder = sources["codec1"], tmp / "out-codec1"
        before = os.stat(copies["codec1"]).st_mtime_ns
        check(flat.state(str(src), str(folder))["kind"] == "done", "the copy made is not seen as done")
        s = flat.convert(str(src), str(folder))
        check(s["reused"] and os.stat(copies["codec1"]).st_mtime_ns == before, "the copy made before was not taken again")
        os.utime(src, (1700000000, 1700000000))   # (the source changed: copied again)
        check(flat.state(str(src), str(folder))["kind"] == "stale", "a changed source's copy is taken as its")
        s = flat.convert(str(src), str(folder))
        check(not s["reused"], "a changed source's copy was taken again")
        flat_ok(s["path"], str(src))
        # stopped, then gone on with: the same copy as a fresh one
        fresh = copies["codec1"].read_bytes()
        folder = tmp / "out-resume"
        calls = [0]
        old_cp = flat.CHECKPOINT_S
        flat.CHECKPOINT_S = 0   # (a checkpoint after every file)

        def stop_after():
            calls[0] += 1
            return calls[0] > 40
        try:
            flat.convert(str(src), str(folder), cancel=stop_after)
            raise AssertionError("the stop was not heard")
        except flat.FlatError as e:
            check(e.code == "flat-cancelled", "stopping said %s" % e.code)
        finally:
            flat.CHECKPOINT_S = old_cp
        st = flat.state(str(src), str(folder))
        check(st["kind"] == "partial" and st["written"] > 0 and not Path(st["dst"]).exists(), "a stopped conversion: %s" % st["kind"])
        with open(st["part"], "r+b") as f:   # (what a stop leaves past its checkpoint: rubbish, written again)
            f.seek(st["written"])
            f.write(b"\xee" * 5000)
        s = flat.convert(str(src), str(folder))
        check(s["resumed_from"] == st["written"] and Path(s["path"]).read_bytes() == fresh, "going on did not end as a fresh copy")
        # ---- refusals ----
        folder = tmp / "out-space"
        old_free = flat.free_bytes
        flat.free_bytes = lambda folder: 10 << 20
        try:
            flat.convert(str(sources["framed"]), str(folder))
            raise AssertionError("no space and no refusal")
        except flat.FlatError as e:
            check(e.code == "flat-space" and "GB" in e.message, "too little space said %s: %s" % (e.code, e.message))
        finally:
            flat.free_bytes = old_free
        check(not any(folder.iterdir()), "too little space left files behind: %s" % list(folder.iterdir()))
        # the disk full halfway: said as such, and the next conversion goes on
        folder = tmp / "out-full"
        old_write, written = flat.Sink.write, [0]

        def full(self, b):
            written[0] += len(b)
            if written[0] > 2 << 20:
                raise OSError(28, "No space left on device")
            old_write(self, b)
        flat.Sink.write = full
        flat.CHECKPOINT_S = 0
        try:
            flat.convert(str(src), str(folder))
            raise AssertionError("a full disk was not heard")
        except flat.FlatError as e:
            check(e.code == "flat-space" and "full" in e.message, "a full disk said %s: %s" % (e.code, e.message))
        finally:
            flat.Sink.write, flat.CHECKPOINT_S = old_write, old_cp
        check(flat.state(str(src), str(folder))["kind"] == "partial", "a full disk's copy does not go on")
        s = flat.convert(str(src), str(folder))
        check(s["resumed_from"] > 0 and Path(s["path"]).read_bytes() == fresh, "the copy after a full disk is not a fresh one")
        folder = tmp / "out-other"
        folder.mkdir()
        (folder / "game.nup").write_bytes(b"someone's own file")
        check(flat.state(str(src), str(folder))["kind"] == "other", "a game.nup not ours is not seen as such")
        try:
            flat.convert(str(src), str(folder))
            raise AssertionError("a game.nup not ours was overwritten")
        except flat.FlatError as e:
            check(e.code == "flat-other" and (folder / "game.nup").read_bytes() == b"someone's own file", "said %s" % e.code)
        own = tmp / "own"   # (the player's dump itself named game.nup, in the folder asked for the copy)
        own.mkdir()
        shutil.copyfile(src, own / "game.nup")
        try:
            flat.convert(str(own / "game.nup"), str(own))
            raise AssertionError("the source itself was taken as the copy's place")
        except flat.FlatError as e:
            check(e.code == "flat-other", "the source as the copy's place said %s" % e.code)
        # a damaged frame: stopped, with the file's name
        bad = tmp / "bad.nup"
        files = tree("codec1")
        frame, size, codec = files["romfs/AI/x.ainb"]
        files["romfs/AI/x.ainb"] = (frame[:-9] + bytes(9), size, codec)
        make_nup(bad, [dict(id=BASE_ID, version=VERSION, meta=b"META", files=files)])
        try:
            flat.convert(str(bad), str(tmp / "out-bad"))
            raise AssertionError("a damaged frame was copied")
        except flat.FlatError as e:
            check(e.code == "flat-failed" and "x.ainb" in e.message, "a damaged frame said %s: %s" % (e.code, e.message))
        # a file the dump holds otherwise than its record's SHA-256 says: copied as extracted, named
        odd = tmp / "odd.nup"
        files = tree("codec1")
        f = files["romfs/AI/x.ainb"]
        files["romfs/AI/x.ainb"] = (f[0], f[1], f[2], len(f[0]), bytes(32))   # (a record whose SHA-256 is not its content's)
        make_nup(odd, [dict(id=BASE_ID, version=VERSION, meta=b"META", files=files)])
        s = flat.convert(str(odd), str(tmp / "out-odd"))
        m = flat.read_mark(s["path"] + flat.MARK_SUFFIX)
        check(s["damaged"] == 1 and list(m["damaged"]) == ["romfs/AI/x.ainb"], "the file unlike its record is not named: %s" % m["damaged"])
        # a .zs of the game the dump read as zeros (a player's dump on 25/11/2026: the Water Temple's gate texture, among 9 files),
        # under codec 1, its record made from the zeros: copied as it is, and named
        zeroed = tmp / "zeroed.nup"
        files = tree("codec1")
        broken = bytes(4096) + GAME["romfs/Pack/Actor/Link.pack.zs"][4096:]
        files["romfs/Pack/Actor/Link.pack.zs"] = as_codec1(broken)
        make_nup(zeroed, [dict(id=BASE_ID, version=VERSION, meta=b"META", files=files)])
        s = flat.convert(str(zeroed), str(tmp / "out-zeroed"))
        check(s["not_zstd"] == ["romfs/Pack/Actor/Link.pack.zs"] and s["damaged"] == 0, "the zeroed .zs is not named: %s" % s)
        got, _ = extract_all(s["path"])
        check(got["romfs/Pack/Actor/Link.pack.zs"] == broken, "the zeroed .zs is not copied as the dump has it")
        # ---- the console's reader, as it is, on this PC ----
        build = tmp / "c"
        build.mkdir()
        exe = build / "nup_same"
        run([a.cc, "-O1", "-D_GNU_SOURCE", "-I" + str(ROOT / "host"), str(ROOT / "scripts" / "ps5" / "nup" / "nup_same.c"),
             str(ROOT / "host" / "nup_fs.c"), "-lpthread", "-o", str(exe)])
        for kind, copy in list(copies.items()) + [("two", Path(tmp / "out-two" / "game.nup"))]:
            out = run([str(exe), str(ref), str(copy), "--threads", "4", "--parts", "24", "--crowd", "200"])
            check("RESULT: PASS" in out, "%s: the console's reader does not read the copy as the flat reference:\n%s" % (kind, out[-2000:]))
            check("stored compressed" not in out and "other info: packed 0, bad 0" in out, "%s: the copy, as the console sees it:\n%s"
                  % (kind, out[-1500:]))
        # the copy against what nx2/nup.py extracts from the source, read through the console's reader (nup_same --hashes: what
        # was run on a player's real dump)
        for kind in ("codec1", "framed"):
            lst = tmp / ("%s.list" % kind)
            run([sys.executable, "-I", str(ROOT / "scripts" / "ps5" / "nup" / "nup_hashes.py"), str(sources[kind]), str(lst), "--record"])
            out = run([str(exe), "--hashes", str(lst), str(copies[kind])])
            check("RESULT: PASS" in out and "package info: packed 0, bad 0" in out, "%s: the copy against the source's extraction:\n%s"
                  % (kind, out[-1500:]))
        r = subprocess.run([str(exe), "--hashes", str(tmp / "codec1.list"), str(sources["codec1"])], capture_output=True, text=True)
        check(r.returncode and "the layer does not read it" in r.stdout, "the codec 1 source passes the list:\n%s" % r.stdout[-1500:])
        # and the sources themselves, as the console sees them: refused, or files it cannot open
        r = subprocess.run([str(exe), str(ref), str(sources["codec1"]), "--crowd", "0"], capture_output=True, text=True)
        check(r.returncode and "stored compressed" in r.stdout and "not readable in the other" in r.stdout,
              "the codec 1 source is not what the console could not read:\n%s" % r.stdout[-1500:])
        # (a frame of itself whose record says the frame's size: a file with bad bytes there; one that says the file's size: the
        # console's sample refuses the package at open)
        r = subprocess.run([str(exe), str(ref), str(sources["framed"]), "--crowd", "0"], capture_output=True, text=True)
        check(r.returncode and "not readable in the other: its bytes are outside the package" in r.stdout,
              "the framed source (stored size: the frame's) is read by the console:\n%s" % r.stdout[-1500:])
        r = subprocess.run([str(exe), str(ref), str(sources["framed-size"]), "--crowd", "0"], capture_output=True, text=True)
        check(r.returncode and "frames of themselves" in r.stdout, "the framed source is not refused by the console:\n%s" % r.stdout[-1500:])
    print("PASS: the flat copy of compressed .nup files (codec 1, frames of themselves, two titles): what nx2/nup.py extracts, flat, "
          "gone on with, taken again, refused without room; read by host/nup_fs.c as a flat dump")


if __name__ == "__main__":
    main()
