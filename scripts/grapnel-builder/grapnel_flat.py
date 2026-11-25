# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel: the flat copy of a .nup whose game files are stored compressed, which is what the console is given.

The console reads the game file in place (host/nup_fs.c: one descriptor, no file made from it) and opens no compressed file. Some
dumpers keep files compressed: codec 1 in a file's record (about a fifth of the game's files in such dumps), or, from newer ones,
every file as a zstd frame of itself with codec 0 (nx2/nup.py is_packed). Such a .nup builds (the builder reads it with nx2/nup.py,
which unpacks those), but the game it sends could not start: the console read the compressed bytes as the files.

So the builder makes, once, on the player's PC, a flat copy: a .nup with the same game, every file as itself (codec 0, its stored
size its size), which the console reads as it reads a normal dump. What is in it:
  - one title, the newest (nx2/nup.py pick_title, which the console and the builder read): its root, its exefs/ (the game's
    program, ~40 MB: so that the copy is a whole game file the builder takes too) and its romfs/. The base game's title, which
    nothing reads once the update is there, is left out (on the port's own dump: 0.35 GB of files no one reads, and the TOC's
    records for 186,000 files more);
  - each file as nx2/nup.py extracts it (copy_to: codec 1 unpacked, a frame of itself unpacked, a .zs file of the game, zstd in
    the game itself, as it is), in the order of the source, files with the same bytes once (as in the dump);
  - the header, the TOC (folders, files, the child list, the names of that title alone) and the title's metadata as the dumper
    wrote them, with the offsets, the stored sizes, the TOC's SHA-256 and the sizes made right. Each record keeps its SHA-256 (that
    of the file's content).
Written as <folder>/game.nup.part, then checked: every file read back and its SHA-256 compared with its record's (or, for a file
whose content in the dump is not its record's, with what nx2/nup.py extracted: the copy says so), then renamed game.nup. Beside it,
game.nup.grapnel.json says from what (the source's name, size and date, as step 1 stamps it, and the TOC's SHA-256 the copy has),
how far it is, and what was found. A conversion stopped (cancelled, the PC off) goes on from its last checkpoint; a copy made
before from the same file, whole and checked, is taken again after a check of its size and its TOC (the same as this source
gives). A normal .nup is never copied: needs_flat() says no.

    import grapnel_flat
    info = grapnel_flat.inspect(path)                 # what is compressed (the console's sample, and the codec 1 records)
    if grapnel_flat.needs_flat(info):
        st = grapnel_flat.state(path, folder)         # "done" (reuse), "partial" (resume), "none", "other" (not ours)
        grapnel_flat.convert(path, folder, progress=..., info=..., cancel=...)   # -> the copy's path and a summary
"""
import errno
import hashlib
import json
import os
import shutil
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
NX2 = os.path.join(os.path.dirname(os.path.dirname(HERE)), "nx2")
if NX2 not in sys.path:
    sys.path.insert(0, NX2)
import nup  # noqa: E402

CONVERTER = 1               # (the copy's layout: a copy made by another version of this is made again)
FLAT_NAME = "game.nup"
MARK_SUFFIX = ".grapnel.json"
PART_SUFFIX = ".part"
ALIGN = 16
SAMPLE = 64                 # (the files host/nup_fs.c samples at open for frames of themselves: the same here)
CHECKPOINT_BYTES = 2 << 30  # (the copy flushed and its progress written: a stopped conversion goes on from there)
CHECKPOINT_S = 20
SPACE_MARGIN = 256 << 20    # (room left on the disk beyond the copy)


class FlatError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code, self.message = code, message


def stamp(path):
    """The source's fingerprint, as step 1 stamps it: its name, size and modification time"""
    st = os.stat(path)
    return "%s %d %d" % (os.path.basename(path), st.st_size, int(st.st_mtime))


def _align(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def is_zstd_frame_of_itself(p, r):
    """(the console's sample, host/nup_fs.c) the file's first bytes are a zstd frame whose stated content size is the file's"""
    p.f.seek(r[3])
    return nup.frame_content_size(p.f.read(18)) == r[2]


def inspect_title(p, t):
    """What of the title's romfs the console could not read in place: {"coded": records with a codec (1: zstd), "framed": files
    among those sampled that are zstd frames of themselves (codec 0, a newer dumper's), "sampled", "files", "bytes" (the romfs's
    files' sizes), "coded_bytes"}. The sample is the console's (host/nup_fs.c nup_open): 64 files spread over the tree, not .zs
    ones (zstd in the game too)"""
    if t["romfs"] == 0xffffffff:
        return {"coded": 0, "framed": 0, "sampled": 0, "files": 0, "bytes": 0, "coded_bytes": 0}
    files = list(p.walk(t["romfs"]))
    coded = [r for _, r in files if r[5] != 0]
    step = max(1, len(files) // SAMPLE)
    sampled = framed = 0
    for i in range(0, len(files), step):
        if sampled >= SAMPLE:
            break
        for rel, r in files[i:i + step]:
            if r[5] != 0 or r[2] < 18 or rel.endswith(".zs"):
                continue
            sampled += 1
            framed += is_zstd_frame_of_itself(p, r)
            break
    return {"coded": len(coded), "framed": framed, "sampled": sampled, "files": len(files),
            "bytes": sum(r[2] for _, r in files), "coded_bytes": sum(r[2] for r in coded),
            "unknown_codec": sum(1 for r in coded if r[5] != 1)}


def inspect(path):
    """inspect_title() of the newest title of the .nup at path, with its id and version"""
    p = nup.Nup(path)
    try:
        t = nup.pick_title(p, None)
        info = inspect_title(p, t)
        info.update(id=t["id"], version=t["version"])
        return info
    finally:
        p.f.close()


def needs_flat(info):
    """Whether the console needs a flat copy of this package (a normal one: never)"""
    return bool(info and (info.get("coded") or info.get("framed")))


# ---- the copy's layout, made from the source's TOC alone ----

class Layout:
    """The flat copy of the newest title: its header, TOC and metadata (bytes), and each extent's place in it"""

    def __init__(self, p, t):
        self.title = t
        nd_old = p.nd
        # the folders under the title's root, breadth first (each once: nup.py's strict check refused a cycle already)
        order, seen, paths = [t["root"]], {t["root"]: 0}, [""]
        i = 0
        while i < len(order):
            no, nl, first, count = p.dirs[order[i]]
            for k in p.kids[first:first + count]:
                if k < nd_old:
                    if k in seen:
                        raise FlatError("nup-damaged", "a folder is reached twice in the .nup's tree")
                    seen[k] = len(order)
                    order.append(k)
                    cno, cnl = p.dirs[k][:2]
                    paths.append(paths[i] + p.name(cno, cnl) + "/" if i else p.name(cno, cnl) + "/")
            i += 1
        nd = len(order)
        strings, at_name = bytearray(), {}

        def name_at(off, n):
            b = bytes(p.strings[off:off + n])
            if b not in at_name:
                at_name[b] = len(strings)
                strings.extend(b)
            return at_name[b]
        dirs, kids, files, fidx, fpath, fname = bytearray(), [], [], {}, [], []
        for di, d in enumerate(order):
            no, nl, first, count = p.dirs[d]
            dirs += struct.pack("<4IQ", name_at(no, nl), nl, len(kids), count, 0)
            for k in p.kids[first:first + count]:
                if k < nd_old:
                    kids.append(seen[k])
                    continue
                f = k - nd_old
                if f not in fidx:
                    fidx[f] = len(files)
                    files.append(f)
                    r = p.files[f]
                    fpath.append(paths[di] + p.name(r[0], r[1]))
                    fname.append(name_at(r[0], r[1]))   # (every name in the table before its size is known)
                kids.append(nd + fidx[f])
        self.nd, self.nf, self.paths = nd, len(files), fpath
        # the metadata blob the title's record points at (the dumper's: kept as it is, after the TOC)
        p.f.seek(0xb0 + 0x40 * t["index"])
        trec = bytearray(p.f.read(0x40))
        moff, msz = struct.unpack_from("<QQ", trec, 0x20)
        self.meta = b""
        if moff and msz:
            p.f.seek(moff)
            self.meta = p.f.read(msz)
            if len(self.meta) != msz:
                raise FlatError("nup-damaged", "the .nup ends inside its title's metadata")
        soff = 0x30 + 0x40 + 24 * nd + 80 * len(files) + 4 * len(kids)
        toc_size = _align(soff + len(strings))
        meta_at = _align(0x80 + toc_size)
        self.data_at = _align(meta_at + len(self.meta)) if self.meta else meta_at
        # the extents: each distinct (offset, stored size, codec, size) of the source once, in the source's order
        by_key = {}
        for i, f in enumerate(files):
            r = p.files[f]
            if r[2]:
                by_key.setdefault((r[3], r[4], r[5], r[2]), []).append(i)
        self.extents = []   # (out offset, size, source record, the first file's index in the copy, every file's index)
        at = self.data_at
        for key in sorted(by_key, key=lambda k: (k[0], k[1], k[2], k[3])):
            idx = by_key[key]
            self.extents.append((at, key[3], p.files[files[idx[0]]], idx[0], idx))
            at = _align(at + key[3])
        self.size = (self.extents[-1][0] + self.extents[-1][1]) if self.extents else self.data_at
        self.logical = sum(e[1] for e in self.extents)
        out_at = [0] * len(files)
        for e in self.extents:
            for i in e[4]:
                out_at[i] = e[0]
        recs = bytearray()
        self.hashes = []
        for i, f in enumerate(files):
            r = p.files[f]
            digest, flags, reserved = p.file_details[f]
            self.hashes.append(digest)
            recs += struct.pack("<IIQQQ", fname[i], r[1], r[2], out_at[i] if r[2] else 0, r[2])
            recs += digest + bytes([0, flags]) + reserved
        # the TOC: its header (the dumper's last 8 bytes kept), the one title, folders, files, children, names
        p.f.seek(0x80)
        toc_head = p.f.read(0x30)
        struct.pack_into("<III", trec, 8, 0, order.index(t["exefs"]) if t["exefs"] != 0xffffffff else 0xffffffff,
                         order.index(t["romfs"]) if t["romfs"] != 0xffffffff else 0xffffffff)
        struct.pack_into("<QQ", trec, 0x18, 0, meta_at if self.meta else 0)
        toc = bytearray(b"TOC\x01" + struct.pack("<IQQQQ", 1, nd, len(files), soff, len(strings)) + toc_head[0x28:0x30])
        toc += trec + dirs + recs + struct.pack("<%dI" % len(kids), *kids) + strings
        toc += bytes(toc_size - len(toc))
        assert len(toc) == toc_size and soff == 0x30 + 0x40 + len(dirs) + len(recs) + 4 * len(kids)
        self.toc = bytes(toc)
        self.toc_sha256 = hashlib.sha256(self.toc).digest()
        p.f.seek(0)
        head = bytearray(p.f.read(0x80))
        titles_in_head = struct.unpack_from("<Q", head, 0x48)[0]
        struct.pack_into("<QQ", head, 0x08, 0x80, toc_size)
        head[0x18:0x38] = self.toc_sha256
        struct.pack_into("<QQ", head, 0x38, self.data_at, self.size)
        if titles_in_head == len(p.titles):   # (+0x48: the titles' count in the dumps seen; anything else is left as it is)
            struct.pack_into("<Q", head, 0x48, 1)
        self.head = bytes(head)
        self.meta_at = meta_at

    def prefix(self):
        """Everything before the files' bytes: the header, the TOC, the metadata, with the padding"""
        b = bytearray(self.data_at)
        b[0:0x80] = self.head
        b[0x80:0x80 + len(self.toc)] = self.toc
        if self.meta:
            b[self.meta_at:self.meta_at + len(self.meta)] = self.meta
        return bytes(b)


def paths_for(folder):
    dst = os.path.join(folder, FLAT_NAME)
    return dst, dst + PART_SUFFIX, dst + MARK_SUFFIX


def read_mark(path):
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
        return m if isinstance(m, dict) else None
    except (OSError, ValueError):
        return None


def write_mark(path, mark):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(mark, f, indent=1)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def state(src, folder):
    """What the folder holds for this source, without reading the copy: {"kind": "done" (a copy of it, whole and checked: taken
    again after reuse_check()), "partial" (a conversion of it stopped: it goes on), "stale" (a copy or a conversion of another file,
    or of another version of it: made again), "other" (a game.nup there that this builder did not make, or the source itself:
    another folder is needed), "none"}, with the paths, the mark, and the bytes already written"""
    dst, part, mark_path = paths_for(folder)
    m = read_mark(mark_path)
    out = {"dst": dst, "part": part, "mark_path": mark_path, "mark": m, "written": 0, "kind": "none"}
    same_file = os.path.exists(dst) and os.path.exists(src) and os.path.samefile(dst, src)
    if same_file or (os.path.exists(dst) and not m):
        out["kind"] = "other"
        return out
    if not m:
        return out
    ours = m.get("converter") == CONVERTER and m.get("source", {}).get("stamp") == stamp(src)
    if ours and m.get("state") == "done" and os.path.isfile(dst) and os.path.getsize(dst) == m.get("size"):
        out["kind"] = "done"
    elif ours and m.get("state") in ("converting", "written") and os.path.isfile(part):
        out["kind"] = "partial"
        out["written"] = min(int(m.get("written", 0)), os.path.getsize(part))
    elif os.path.exists(dst) or os.path.exists(part) or m:
        out["kind"] = "stale"
    return out


def reuse_check(dst, mark, toc_sha256=None):
    """A copy made before, checked again without reading its files: its size, its header, its TOC's SHA-256 (the copy's own,
    and the one this source gives when given), one title, every file flat (codec 0, its stored size its size). "" when good, else
    what is wrong"""
    try:
        with open(dst, "rb") as f:
            head = f.read(0x80)
            toc_at, toc_size = struct.unpack_from("<QQ", head, 8)
            f.seek(toc_at)
            toc = f.read(toc_size)
        if head[:4] != b"NUP\x01" or len(toc) != toc_size or hashlib.sha256(toc).digest() != head[0x18:0x38]:
            return "its TOC does not match its header"
        if toc_sha256 is not None and head[0x18:0x38] != toc_sha256:
            return "it was made from another file, or by another version of the builder"
        if hex_of(head[0x18:0x38]) != mark.get("toc_sha256") or os.path.getsize(dst) != mark.get("size"):
            return "it is not the copy its notes describe"
        nt, nd, nf = struct.unpack_from("<IQQ", toc, 4)
        files_at = 0x30 + 0x40 * nt + 24 * nd
        for i in range(nf):
            size, _, stored = struct.unpack_from("<QQQ", toc, files_at + 80 * i + 8)
            if toc[files_at + 80 * i + 64] != 0 or stored != size:
                return "a file in it is not stored flat"
    except (OSError, struct.error) as e:
        return "it cannot be read (%s)" % e
    return ""


def hex_of(b):
    return b.hex() if b is not None else None


def need_bytes(layout, written=0):
    """The disk space the conversion still needs: the copy, less what a stopped one wrote, and a margin"""
    return max(0, layout.size - written) + SPACE_MARGIN


def free_bytes(folder):
    p = os.path.abspath(folder)
    while p and not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            break
        p = up
    return shutil.disk_usage(p).free


class Sink:
    """Where nx2/nup.py's copy_to writes a file: the copy, at its place, with its SHA-256 and its length"""

    def __init__(self, f):
        self.f, self.h, self.n, self.head = f, hashlib.sha256(), 0, b""

    def write(self, b):
        self.f.write(b)
        self.h.update(b)
        if len(self.head) < 4:
            self.head += bytes(b[:4 - len(self.head)])
        self.n += len(b)


def source_toc_ok(p):
    """Whether the source's TOC is what its header's SHA-256 says (a dump whose header gives none, or no TOC size: not said)"""
    if not p.toc_size or not any(p.toc_sha256):
        return None
    p.f.seek(p.toc_offset)
    h, left = hashlib.sha256(), p.toc_size
    while left:
        b = p.f.read(min(left, 8 << 20))
        if not b:
            return False
        h.update(b)
        left -= len(b)
    return h.digest() == p.toc_sha256


def plan(src):
    """(the open source, its newest title, the copy's Layout). The source is read as the builder reads it (nx2/nup.py, not its
    strict reading: a dumper's header fields it does not need are not asked for); the copy's own TOC is made whole"""
    try:
        p = nup.Nup(src)
    except SystemExit as e:
        raise FlatError("nup-damaged", str(e))
    try:
        t = nup.pick_title(p, None)
        return p, t, Layout(p, t)
    except (IndexError, struct.error) as e:
        p.f.close()
        raise FlatError("nup-damaged", "the .nup's TOC is damaged (%s)" % e)
    except BaseException:
        p.f.close()
        raise


def convert(src, folder, progress=None, info=None, cancel=None, verify=True):
    """The flat copy of src in folder/game.nup: made (or gone on with, or taken again), checked. -> a summary dict (path, size,
    files, unpacked: codec 1 and frames, seconds, reused). FlatError on a failure (code: flat-space, flat-other, nup-damaged,
    flat-failed, flat-verify, flat-cancelled)"""
    try:
        return _convert(src, folder, progress, info, cancel, verify)
    except OSError as e:   # (the disk full halfway, a folder that cannot be written, a drive gone: said, and it goes on next time)
        if e.errno == errno.ENOSPC:
            raise FlatError("flat-space", "the disk of %s became full while the copy was written: free room there (or choose a "
                            "folder on another disk), then make it again: it goes on where it stopped" % folder)
        raise FlatError("flat-failed", "the copy could not be written or read in %s: %s" % (folder, e))


def _convert(src, folder, progress, info, cancel, verify):
    progress = progress or (lambda fraction, text: None)
    info = info or (lambda text: None)
    os.makedirs(folder, exist_ok=True)
    t0 = time.time()
    st = state(src, folder)
    if st["kind"] == "other":
        raise FlatError("flat-other", "%s is already there and the builder did not make it (or it is your dump itself): choose "
                        "another folder for the copy" % st["dst"])
    p, t, lay = plan(src)
    try:
        if st["kind"] == "done":
            why = reuse_check(st["dst"], st["mark"], lay.toc_sha256)
            if not why:
                info("the flat copy made before from this .nup is there and whole: %s (%.2f GB), taken again" % (st["dst"], lay.size / 1e9))
                progress(1.0, "taken again")
                return dict(st["mark"].get("summary") or {}, path=st["dst"], reused=True)
            info("the flat copy there is not taken again (%s): made again" % why)
            st["kind"] = "stale"
        resume = 0
        if st["kind"] == "partial" and st["mark"].get("toc_sha256") == lay.toc_sha256.hex() and st["written"] >= lay.data_at:
            resume = st["written"]
        if st["kind"] in ("stale", "partial") and not resume:
            for f in (st["dst"], st["part"], st["mark_path"]):
                if os.path.exists(f):
                    os.remove(f)
        need, free = need_bytes(lay, resume), free_bytes(folder)
        if free < need:
            raise FlatError("flat-space", "the copy needs %.1f GB more on the disk of %s, which has %.1f GB free" % (
                need / 1e9, folder, free / 1e9))
        mark = {"converter": CONVERTER, "source": {"path": os.path.abspath(src), "stamp": stamp(src)},
                "title": "%016X v%d" % (t["id"], t["version"]), "toc_sha256": lay.toc_sha256.hex(), "size": lay.size,
                "state": "converting", "written": resume or lay.data_at, "damaged": {}, "started": time.strftime("%Y-%m-%d %H:%M:%S")}
        if resume:
            old = st["mark"]
            mark["damaged"] = old.get("damaged", {})
            mark["started"] = old.get("started", mark["started"])
            info("a conversion of this .nup stopped at %.1f of %.1f GB: it goes on from there" % (resume / 1e9, lay.size / 1e9))
        else:
            info("the flat copy: %d files (%d distinct), %.2f GB, into %s" % (lay.nf, len(lay.extents), lay.size / 1e9,
                                                                          st["part"]))
        if not resume and source_toc_ok(p) is False:
            info("note: the .nup's TOC is not what its header's SHA-256 says (the dump may be damaged); the copy's TOC is made anew, "
                 "and every file is checked as it is copied")
        unpacked = {"codec1": 0, "framed": 0}
        with open(st["part"], "r+b" if resume else "wb") as f:
            if not resume:
                f.write(lay.prefix())
            else:   # (what a stop left past its checkpoint goes: the padding after it is zeros again, as in a fresh copy)
                f.truncate(resume)
            write_mark(st["mark_path"], mark)
            done = sum(e[1] for e in lay.extents if e[0] + e[1] <= (resume or 0))
            last_cp, last_cp_t, last_shown = done, time.time(), 0
            for out_at, size, r, first, idx in lay.extents:
                if resume and out_at + size <= resume:
                    continue
                if cancel is not None and cancel():
                    raise FlatError("flat-cancelled", "the conversion was stopped: it goes on from %.1f GB next time" % (mark["written"] / 1e9))
                name = lay.paths[first]
                f.seek(out_at)
                sink = Sink(f)
                try:
                    if r[5] == 1:
                        p.copy_to(r, sink, name, packed=True)
                        unpacked["codec1"] += 1
                    elif r[5] == 0:
                        framed = p.is_packed(r, name)
                        p.copy_to(r, sink, name, packed=None if framed else False)
                        unpacked["framed"] += framed
                    else:
                        raise FlatError("nup-codec", "%s is stored with codec %d, which the builder does not know" % (name, r[5]))
                except SystemExit as e:
                    raise FlatError("flat-failed", "%s could not be read out of the .nup: %s" % (name, e))
                if sink.n != size:
                    raise FlatError("flat-failed", "%s came out of the .nup as %d bytes, %d expected" % (name, sink.n, size))
                if sink.h.digest() != lay.hashes[first]:
                    mark["damaged"][name] = sink.h.hexdigest()
                if name.startswith("romfs/") and name.endswith(".zs") and not nup.zstd_start(sink.head):
                    # (a .zs of the game that does not start as zstd: zeros where the game has data, a dump's read that failed.
                    # Copied as it is, as the console would read it from the dump; said: the game leaves out what uses it,
                    # nx2/nup.py damaged_game_files, #1002)
                    if name not in mark.setdefault("not_zstd", []):
                        mark["not_zstd"].append(name)
                done += size
                now = time.time()
                if done - last_cp >= CHECKPOINT_BYTES or now - last_cp_t >= CHECKPOINT_S:
                    f.flush()
                    os.fsync(f.fileno())
                    mark["written"] = out_at + size
                    write_mark(st["mark_path"], mark)
                    last_cp, last_cp_t = done, now
                if now - last_shown >= 0.25:
                    progress(0.8 * done / max(1, lay.logical), "%.1f of %.1f GB" % (done / 1e9, lay.logical / 1e9))
                    last_shown = now
            f.truncate(lay.size)
            f.flush()
            os.fsync(f.fileno())
        mark.update(state="written", written=lay.size)
        write_mark(st["mark_path"], mark)
        if mark.get("not_zstd"):
            info("note: %d .zs files of the game in the .nup do not start as zstd (zeros where the game has data: the dump read "
                 "them wrong). They are copied as they are; the game leaves out what uses them (nx2/nup.py damaged_game_files): "
                 "%s" % (len(mark["not_zstd"]), ", ".join(mark["not_zstd"][:5])))
        if mark["damaged"]:
            info("note: %d files of the .nup are not what their records' SHA-256 say (the dump may be damaged there; copied as "
                 "nx2/nup.py reads them): %s" % (len(mark["damaged"]), ", ".join(sorted(mark["damaged"])[:5])))
        t_write = time.time() - t0
        bad = check_copy(st["part"], lay, mark["damaged"], progress=lambda fr, tx: progress(0.8 + 0.2 * fr, tx), cancel=cancel) \
            if verify else []
        if bad:
            raise FlatError("flat-verify", "the copy read back is not what was written: %s" % "; ".join(bad[:5]))
        os.replace(st["part"], st["dst"])
        summary = {"path": st["dst"], "size": lay.size, "files": lay.nf, "extents": len(lay.extents), "unpacked_codec1": unpacked["codec1"],
                   "unpacked_frames": unpacked["framed"], "damaged": len(mark["damaged"]), "damaged_files": sorted(mark["damaged"]),
                   "not_zstd": list(mark.get("not_zstd", [])), "seconds_write": round(t_write, 1),
                   "seconds": round(time.time() - t0, 1), "verified": bool(verify), "resumed_from": resume, "reused": False}
        mark.update(state="done", finished=time.strftime("%Y-%m-%d %H:%M:%S"), summary=summary)
        write_mark(st["mark_path"], mark)
        progress(1.0, "checked")
        info("the flat copy is made and checked: %s, %.2f GB, %d files (%d unpacked: %d codec 1, %d frames), in %.0f s"
             % (st["dst"], lay.size / 1e9, lay.nf, unpacked["codec1"] + unpacked["framed"], unpacked["codec1"], unpacked["framed"],
                summary["seconds"]))
        return summary
    finally:
        p.f.close()


def check_copy(path, lay, damaged=None, progress=None, cancel=None):
    """Every file of the copy read back: its SHA-256 against its record's (or what was extracted, for a file the dump holds
    otherwise than its record says), its record flat. -> what is wrong (empty: all good)"""
    progress = progress or (lambda fraction, text: None)
    damaged = damaged or {}
    bad = []
    with open(path, "rb") as f:
        if f.read(lay.data_at) != lay.prefix():
            bad.append("its header or TOC")
        done, last = 0, 0
        for out_at, size, r, first, idx in lay.extents:
            if cancel is not None and cancel():
                raise FlatError("flat-cancelled", "the check was stopped")
            f.seek(out_at)
            h, left = hashlib.sha256(), size
            while left:
                b = f.read(min(left, 8 << 20))
                if not b:
                    break
                h.update(b)
                left -= len(b)
            want = damaged.get(lay.paths[first])
            if left or h.hexdigest() != (want or lay.hashes[first].hex()):
                bad.append(lay.paths[first])
            done += size
            if time.time() - last >= 0.25:
                progress(done / max(1, lay.logical), "checking: %.1f of %.1f GB" % (done / 1e9, lay.logical / 1e9))
                last = time.time()
    return bad
