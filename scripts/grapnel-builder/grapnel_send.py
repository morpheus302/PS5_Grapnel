#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel: the title sent to the console (the assistant's step 6), over FTP.

The way: FTP, which every jailbroken PS5 setup can run. If an FTP server already answers on the console (ftpsrv's port 2121),
it is used; otherwise the builder sends ftpsrv (ps5-payload-dev/ftpsrv, GPL-3.0-or-later, built from its source with the
toolchain's payload SDK: toolchain/ftpsrv/) to the console's ELF loader (port 9021), as the title sends its broker. FTP needs
nothing more on the PC than Python's ftplib, and ftpsrv's own commands are known: USER without a password, STOR after REST
writes from that offset (an interrupted send goes on), SIZE, MLSD of the current folder, RETR, MKD, DELE, RMD, and SELF,
which turns its on-the-fly SELF decryption off so that a file comes back as it is.

Where it goes (place): the console's internal storage, the title in /data/homebrew/GRPN00001 and the game file in
/data/grapnel/game.nup, or an extension disk mounted at /mnt/ext1 (or /mnt/ext0), the title in /mnt/ext1/homebrew/GRPN00001
(where ShadowMountPlus registers the titles it finds) and the game file in /mnt/ext1/grapnel/game.nup. The rest of the data
folder (saves, settings, logs) stays in /data/grapnel on either. When the game file is on an extension disk, the
hookshot_env.txt sent to /data/grapnel says where, HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup (the title reads it there);
on the internal storage it has no such line. ftpsrv has no copy and no free space command, and its RNTO is a rename(), which
fails between /data and an extension disk: a game file at the other place is sent again from the PC, not moved there.

What it touches on the console: the title's folder and the game's folder at each place, and /data/grapnel, nothing else
(_check_write; /data/homebrew and /mnt/extN/homebrew are made when missing, never written into: the other titles there are
not touched). A title or a game file left at the other place is removed only when asked, after the send was read back.
Before anything is replaced, the player's saves (/data/grapnel/user, or /data/hookshot/user for an older install) are
copied to the PC, and the send stops if that fails. What the console already has is known from a journal on the PC
(<out>/console-sent.json, per console address): each file sent whole, with its size and CRC32. A file is kept when the
journal has it with the size and CRC32 of the PC's file and the console has it with that size (a new eboot.bin is as long as
the last one: the size alone would keep the old); a file the journal has as begun with this content and the console has
shorter is gone on with from its end; any other is sent whole. The title's files and folders the new build does not have are
removed (in the title's folder only: /data/grapnel keeps the saves), except its radv-shader-cache/ when the build has none:
on a PC that does not prepare the shaders (a processor without AVX2), the console prepares them itself into that folder
(about an hour) and keeps its progress beside the game; a send of a new build leaves that work where it is. The .nup goes to the place's game.nup only when it is
not there with the same size (the builder checked it is the game; its 26 GB are not read for a CRC). The title folder goes
last, eboot.bin at its very end, so that ShadowMountPlus (which registers what it finds in a homebrew folder once it has
stopped changing) never sees half a title.

    python3 grapnel_send.py --host <console address> --out <output folder> [--nup F] [--place internal|/mnt/ext1]
                            [--flat-dir D] [--probe] [--root R] [--port N]

The game file sent is the one the console can read: the .nup itself, or, when its game files are stored compressed (some
dumpers'), the flat copy the builder made of it (grapnel_flat.py: in the output folder, or --flat-dir, or where the build's
manifest.json says): console_game_file(). A compressed .nup whose copy is not made is not sent (the console would stop as Play
is pressed).

(--root: a folder of this PC standing for the console's /, for tests against a local FTP server.)
"""
import argparse
import datetime
import ftplib
import json
import os
import socket
import sys
import threading
import time
import zlib

FTP_PORT = 2121
ELFLDR_PORT = 9021
TITLE_ID = "GRPN00001"
REMOTE_DATA = "/data/grapnel"
REMOTE_SAVES = ("/data/grapnel/user", "/data/hookshot/user")
INTERNAL = "internal"
EXT_VOLUMES = ("/mnt/ext0", "/mnt/ext1")   # (the console's extension disks, as ShadowMountPlus finds them)
PLACES = (INTERNAL,) + EXT_VOLUMES
NUP_ENV = "HOOKSHOT_PS5_NUP"   # (the title's setting: where game.nup is when it is not /data/grapnel/game.nup)


def place_paths(place):
    """(the title's folder, the game file) at a place: INTERNAL, or an extension disk's mount point"""
    if place in (INTERNAL, None, ""):
        return "/data/homebrew/" + TITLE_ID, REMOTE_DATA + "/game.nup"
    if place not in EXT_VOLUMES:
        raise SendError("no such place: %s" % place)
    return place + "/homebrew/" + TITLE_ID, place + "/grapnel/game.nup"


REMOTE_TITLE, REMOTE_NUP = "/data/homebrew/" + TITLE_ID, REMOTE_DATA + "/game.nup"   # (the internal storage's)
WRITABLE = (REMOTE_TITLE, REMOTE_DATA) + tuple(v + d for v in EXT_VOLUMES for d in ("/homebrew/" + TITLE_ID, "/grapnel"))
PARENTS = ("/data/homebrew",) + tuple(v + "/homebrew" for v in EXT_VOLUMES)   # (made when missing; nothing written in them)
BLOCK = 1 << 20


class SendError(Exception):
    pass


class Cancelled(SendError):
    pass


def console_game_file(nup, out, flat_dir=None):
    """The game file the console is given for the player's .nup: (path, note). A normal .nup: itself. One whose game files are
    stored compressed: the flat copy the builder made of it (grapnel_flat.py), in flat_dir, where the build's manifest.json says,
    or in the output folder. SendError when there is none: the console cannot read the .nup itself"""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:   # (python -I leaves the script's folder off)
        sys.path.insert(0, here)
    import grapnel_flat
    if not grapnel_flat.needs_flat(grapnel_flat.inspect(nup)):
        return nup, ""
    folders = [flat_dir] if flat_dir else []
    try:
        with open(os.path.join(out, "manifest.json"), encoding="utf-8") as f:
            g = json.load(f).get("game_file") or {}
        if g.get("flat") and g.get("path"):
            folders.append(os.path.dirname(g["path"]))
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    folders.append(out)
    for d in folders:
        if d:
            st = grapnel_flat.state(nup, d)
            if st["kind"] == "done":
                return st["dst"], "the .nup keeps game files compressed: its flat copy %s is sent in its place" % st["dst"]
    raise SendError("%s keeps game files compressed, which the console cannot read, and its flat copy is not made: make it first "
                    "(the window's step after the game, or grapnel_build.py --flatten --nup <it> --flat-dir <folder>)" % nup)


def probe(host, timeout=3.0, port=FTP_PORT):
    """What answers on the console: {"ftp": the FTP server's greeting or None, "elfldr": the ELF loader's port is open}"""
    found = {"ftp": None, "elfldr": False}
    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            s.settimeout(timeout)
            greeting = s.recv(256).decode("utf-8", "replace").strip()
            found["ftp"] = greeting if greeting.startswith("220") else None
    except OSError:
        pass
    try:
        with socket.create_connection((host, ELFLDR_PORT), timeout=timeout):
            found["elfldr"] = True   # (connected and closed with nothing sent: the loader runs nothing)
    except OSError:
        pass
    return found


def send_elf(host, path, timeout=10.0):
    """An ELF to the console's loader: the whole file over one connection, which the loader runs"""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise SendError("%s is not an ELF file" % path)
    with socket.create_connection((host, ELFLDR_PORT), timeout=timeout) as s:
        s.sendall(data)
        s.shutdown(socket.SHUT_WR)


def wait_ftp(host, seconds=20.0):
    end = time.time() + seconds
    while time.time() < end:
        if probe(host, timeout=1.5)["ftp"]:
            return True
        time.sleep(1)
    return False


def crc32_file(path):
    c = 0
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(BLOCK), b""):
            c = zlib.crc32(block, c)
    return c


def local_tree(top):
    """{relative path with /: size} of the files below top"""
    out = {}
    for d, dirs, files in os.walk(top):
        dirs.sort()
        for n in sorted(files):
            p = os.path.join(d, n)
            out[os.path.relpath(p, top).replace(os.sep, "/")] = os.path.getsize(p)
    return out


class Console:
    """The console's FTP server, as ftpsrv answers (see the module's notes); root: a local folder standing for its / (tests)"""

    def __init__(self, host, port=FTP_PORT, root="", timeout=30.0):
        self.root = root.rstrip("/")
        self.ftp = ftplib.FTP()
        self.ftp.connect(host, port, timeout=timeout)
        r = self.ftp.sendcmd("USER grapnel")
        if r.startswith("331"):   # (a server that asks a password: anonymous)
            self.ftp.sendcmd("PASS grapnel@")
        self.ftp.voidcmd("TYPE I")
        self.raw = False
        r = self.ftp.sendcmd("SELF")   # (ftpsrv: SELF decryption off for this connection, the files as they are)
        if "enabled" in r.lower():
            r = self.ftp.sendcmd("SELF")
        self.raw = "disabled" in r.lower()

    def close(self):
        try:
            self.ftp.quit()
        except (OSError, ftplib.Error, EOFError):
            self.ftp.close()

    def path(self, p):
        return self.root + p

    @staticmethod
    def _check_write(p):
        if "/../" in p + "/" or "/./" in p + "/" or "//" in p or not any(p == w or p.startswith(w + "/") for w in WRITABLE):
            raise SendError("refused: %s is outside %s" % (p, ", ".join(WRITABLE)))

    def isdir(self, p):
        try:
            self.ftp.cwd(self.path(p))
            return True
        except ftplib.error_perm:
            return False

    def entries(self, p):
        """{name: ("dir"|"file", size)} of a folder (ftpsrv's MLSD lists the current folder); {} if it is not there"""
        if not self.isdir(p):
            return {}
        out = {}

        def line(text):
            facts, _, name = text.partition("; ")
            if name in (".", "..") or not name:
                return
            f = dict(x.split("=", 1) for x in facts.split(";") if "=" in x)
            out[name] = (f.get("type", "file"), int(f.get("size", "0") or 0))
        self.ftp.retrlines("MLSD", line)
        return out

    def tree(self, p):
        """{relative path: size} of the files below p ({} if p is not there)"""
        out = {}
        for name, (kind, size) in self.entries(p).items():
            if kind == "dir":
                for rel, s in self.tree(p + "/" + name).items():
                    out[name + "/" + rel] = s
            else:
                out[name] = size
        return out

    def folders(self, p):
        """The folders below p, as relative paths ([] if p is not there)"""
        out = []
        for name, (kind, _) in self.entries(p).items():
            if kind == "dir":
                out.append(name)
                out += [name + "/" + d for d in self.folders(p + "/" + name)]
        return out

    def size(self, p):
        try:
            r = self.ftp.sendcmd("SIZE " + self.path(p))
            return int(r.split()[1])
        except (ftplib.error_perm, IndexError, ValueError):
            return None

    def mkdirs(self, p):
        """p and the folders above it that are not there: only within the title's and the game's folders, and a homebrew
        folder itself (/data/homebrew, /mnt/ext1/homebrew) when there is none yet (where every homebrew title goes); never a
        disk's mount point"""
        self._check_write(p)
        parts = p.strip("/").split("/")
        for i in range(1, len(parts) + 1):
            q = "/" + "/".join(parts[:i])
            if not self.isdir(q):
                if q not in PARENTS:
                    self._check_write(q)
                self.ftp.mkd(self.path(q))

    def download(self, p, local, progress=None):
        os.makedirs(os.path.dirname(local), exist_ok=True)
        with open(local, "wb") as f:
            def take(block):
                f.write(block)
                if progress:
                    progress(len(block))
            self.ftp.retrbinary("RETR " + self.path(p), take, blocksize=BLOCK)

    def upload(self, local, p, offset=0, progress=None, cancel=None):
        self._check_write(p)
        with open(local, "rb") as f:
            f.seek(offset)

            def sent(block):
                if cancel is not None and cancel.is_set():
                    raise Cancelled("cancelled")
                if progress:
                    progress(len(block))
            self.ftp.storbinary("STOR " + self.path(p), f, blocksize=BLOCK, callback=sent, rest=offset or None)

    def delete(self, p):
        self._check_write(p)
        # (voidcmd, not ftplib's delete: that one takes only 250 or 200, and ftpsrv answers DELE with "226 File deleted")
        self.ftp.voidcmd("DELE " + self.path(p))

    def rmdir(self, p):
        self._check_write(p)
        self.ftp.rmd(self.path(p))

    def crc32(self, p):
        c = [0]

        def take(block):
            c[0] = zlib.crc32(block, c[0])
        self.ftp.retrbinary("RETR " + self.path(p), take, blocksize=BLOCK)
        return c[0]


def volumes(console):
    """The extension disks the console has mounted: /mnt/ext0 and /mnt/ext1 when they are folders with something in them
    (a mount point with no disk is empty). ftpsrv tells no free space"""
    return [v for v in EXT_VOLUMES if console.isdir(v) and console.entries(v)]


def where(console, found=None):
    """Where the title and the game file are now: {"title": [places], "nup": {place: size}}"""
    found = volumes(console) if found is None else found
    places = [INTERNAL] + [v for v in EXT_VOLUMES if v in found]
    title = [q for q in places if console.isdir(place_paths(q)[0])]
    nup = {}
    for q in places:
        size = console.size(place_paths(q)[1])
        if size is not None:
            nup[q] = size
    return {"title": title, "nup": nup}


def plan(console, out, nup=None, place=INTERNAL):
    """What a send would do at a place: the title and data folders there, the saves there, the game file there or not; where
    the game will read its file from (nup_at, None when no copy is there or sent); and what is left at the other place
    (title_elsewhere, nup_elsewhere: offered for removal once the send is read back)"""
    found = volumes(console)
    if place not in (INTERNAL, None, "") and place not in found:
        raise SendError("no extension disk at %s on the console" % place)
    place = place or INTERNAL
    title, remote_nup = place_paths(place)
    now = where(console, found)
    p = {"place": place, "title": title, "nup_path": remote_nup, "volumes": found,
         "title_there": console.isdir(title), "data_there": console.isdir(REMOTE_DATA),
         "saves": [s for s in REMOTE_SAVES if console.isdir(s)], "nup": None,
         "title_elsewhere": [place_paths(q)[0] for q in now["title"] if q != place]}
    if nup:
        there = now["nup"].get(place)
        p["nup"] = {"local": os.path.getsize(nup), "there": there, "send": there != os.path.getsize(nup)}
        p["nup_at"] = remote_nup
        p["nup_elsewhere"] = [(place_paths(q)[1], s) for q, s in sorted(now["nup"].items()) if q != place]
    else:   # (the game file is not sent: the game reads the copy it finds, the place's first, else the internal one)
        keep = place if place in now["nup"] else next((q for q in PLACES if q in now["nup"]), None)
        p["nup_at"] = place_paths(keep)[1] if keep else None
        p["nup_elsewhere"] = []
    p["replace"] = [d for d, there in ((title, p["title_there"]), (REMOTE_DATA, p["data_there"])) if there]
    return p


def env_text(local_env, nup_at):
    """hookshot_env.txt as sent: the build's, with no HOOKSHOT_PS5_NUP line, and one naming the game file when it is not
    /data/grapnel/game.nup"""
    try:
        with open(local_env, encoding="utf-8", newline="") as f:
            raw = f.read()
    except OSError:
        raw = ""
    line = nup_at and nup_at != REMOTE_NUP
    lines = raw.splitlines()
    kept = [l for l in lines if not l.strip().startswith(NUP_ENV + "=") and l.strip() != ENV_NOTE]
    if not line and kept == lines:
        return raw   # (nothing to say: the build's file as it is)
    text = "\n".join(kept).rstrip("\n") + "\n" if kept else ""
    if line:
        text += ("\n" if text else "") + "%s\n%s=%s\n" % (ENV_NOTE, NUP_ENV, nup_at)
    return text


ENV_NOTE = "# (the builder's step 6) the game file is on an extension disk"


def data_files(out, nup_at):
    """{relative path: local file} of the data folder as sent: the build's files, hookshot_env.txt made for where the game
    file is (<out>/console-send/hookshot_env.txt)"""
    data_local = os.path.join(out, "data", "grapnel")
    files = {rel: os.path.join(data_local, *rel.split("/")) for rel in local_tree(data_local)}
    env = os.path.join(out, "console-send", "hookshot_env.txt")
    if "hookshot_env.txt" not in files and not (nup_at and nup_at != REMOTE_NUP):
        return files
    os.makedirs(os.path.dirname(env), exist_ok=True)
    text = env_text(files.get("hookshot_env.txt", ""), nup_at)
    old = None
    try:
        with open(env, encoding="utf-8") as f:
            old = f.read()
    except OSError:
        pass
    if old != text:   # (the same text keeps the file as it was)
        with open(env, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    files["hookshot_env.txt"] = env
    return files


def backup_saves(console, out, saves, progress=None):
    """The saves' folders copied below <out>/console-saves/<date>/; every file read back to its size, or SendError"""
    if not saves:
        return None
    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    dest = os.path.join(out, "console-saves", stamp)
    count = 0
    for s in saves:
        for rel, size in console.tree(s).items():
            local = os.path.join(dest, s.strip("/").replace("/", os.sep), *rel.split("/"))
            console.download(s + "/" + rel, local, progress)
            if os.path.getsize(local) != size:
                raise SendError("the save %s/%s came back with %d bytes of %d" % (s, rel, os.path.getsize(local), size))
            count += 1
    return dest, count


class Progress:
    """Bytes sent, the rate over the last seconds and the time left"""

    def __init__(self, total, report):
        self.total, self.done, self.report = total, 0, report
        self.marks = [(time.time(), 0)]
        self.lock = threading.Lock()
        self.current = ""

    def add(self, n):
        with self.lock:
            self.done += n
            now = time.time()
            self.marks.append((now, self.done))
            while len(self.marks) > 2 and now - self.marks[0][0] > 10:
                self.marks.pop(0)
            t0, d0 = self.marks[0]
            rate = (self.done - d0) / (now - t0) if now > t0 else 0.0
            left = (self.total - self.done) / rate if rate > 0 else None
            if self.report:
                self.report(self.done, self.total, rate, left, self.current)


class Journal:
    """<out>/console-sent.json: per console address, the files sent there, whole ({"size", "crc"}) or begun ({"size", "crc",
    "begun": true}: the content being sent)"""

    def __init__(self, out, host):
        self.path, self.host = os.path.join(out, "console-sent.json"), host
        try:
            with open(self.path, encoding="utf-8") as f:
                self.all = json.load(f)
        except (OSError, ValueError):
            self.all = {}
        self.files = self.all.setdefault(host, {})

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.all, f, indent=1, sort_keys=True)
        os.replace(tmp, self.path)

    def mark(self, remote, size, crc, begun=False):
        self.files[remote] = {"size": size, "crc": crc, "begun": True} if begun else {"size": size, "crc": crc}
        self.save()


def send(console, out, nup=None, report=None, cancel=None, log=print, host=None, place=INTERNAL, nup_at=None):
    """The data folder, the game file when it is not there, then the title (eboot.bin last), at a place: kept what the
    journal and the console both have as on the PC, gone on with what was begun with this content, the rest sent whole; then
    the title's leftovers removed. nup_at: where the game will read its file (plan()'s; default: the place's).
    Returns the files sent"""
    journal = Journal(out, host or "console")
    jobs = []
    remote_title, remote_nup = place_paths(place)
    nup_at = nup_at if nup_at is not None or not nup else remote_nup
    title_local = os.path.join(out, "homebrew", TITLE_ID)
    data_local = os.path.join(out, "data", "grapnel")
    for local_top in (data_local, title_local):
        if not os.path.isdir(local_top):
            raise SendError("%s is not there: build the game first" % local_top)
    data = data_files(out, nup_at)
    title_files = local_tree(title_local)
    there = {REMOTE_DATA: console.tree(REMOTE_DATA), remote_title: console.tree(remote_title)}
    for rel in sorted(data):
        jobs.append((data[rel], REMOTE_DATA + "/" + rel, os.path.getsize(data[rel]), there[REMOTE_DATA].get(rel)))
    if nup:
        jobs.append((nup, remote_nup, os.path.getsize(nup), console.size(remote_nup)))
    order = sorted(title_files, key=lambda r: (r == "eboot.bin", r))
    for rel in order:
        jobs.append((os.path.join(title_local, *rel.split("/")), remote_title + "/" + rel, title_files[rel],
                     there[remote_title].get(rel)))
    plan_ = []
    for local, remote, size, t in jobs:
        if remote == remote_nup:   # (the game file by its size alone)
            if t != size:
                plan_.append((local, remote, size, t if t is not None and t < size else 0, None))
            continue
        crc = crc32_file(local)
        known = journal.files.get(remote, {})
        if known.get("size") == size and known.get("crc") == crc and t == size and not known.get("begun"):
            continue
        offset = t if known.get("begun") and known.get("crc") == crc and t is not None and t < size else 0
        plan_.append((local, remote, size, offset, crc))
    todo = plan_
    total = sum(s - o for _, _, s, o, _ in todo)
    prog = Progress(total, report)
    log("send: %d files, %d already there; %.1f GB to send" % (len(jobs), len(jobs) - len(todo), total / 1e9))
    made = set()
    for local, remote, size, offset, crc in todo:
        if cancel is not None and cancel.is_set():
            raise Cancelled("cancelled")
        folder = remote.rsplit("/", 1)[0]
        if folder not in made:
            console.mkdirs(folder)
            made.add(folder)
        prog.current = remote
        if crc is not None:
            journal.mark(remote, size, crc, begun=True)
        console.upload(local, remote, offset, prog.add, cancel)
        if crc is not None:
            journal.mark(remote, size, crc)
    # (the title's files the build no longer has: its folder is the title's alone. But a build with no driver's cache of its
    # own (made on a PC that does not prepare the shaders: the console prepares them itself, into that folder, about an hour,
    # and keeps its progress file beside the game) leaves the console's cache there: removed, the console would have it all
    # to make again, or play with an empty cache its progress file calls finished)
    keep = console_cache_kept(title_files)
    for rel in sorted(set(there[remote_title]) - set(title_files)):
        if keep(rel):
            continue
        console.delete(remote_title + "/" + rel)
        journal.files.pop(remote_title + "/" + rel, None)
    # (and its folders the build does not have, emptied above or empty already: the deepest first)
    mine = {os.path.relpath(d, title_local).replace(os.sep, "/") for d, _, _ in os.walk(title_local)} - {"."}
    for rel in sorted(set(console.folders(remote_title)) - mine, key=lambda r: (-r.count("/"), r)):
        if keep(rel):
            continue
        console.rmdir(remote_title + "/" + rel)
    journal.save()
    return [r for _, r, _, _, _ in todo]


CONSOLE_CACHE = "radv-shader-cache"   # (the console driver's cache folder, in the title's: /app0/radv-shader-cache)


def console_cache_kept(title_files):
    """Whether a path of the console's title folder is the driver's cache the console made itself, which a send keeps: only
    when the build sent has no cache of its own (title_files: the build's title files, relative)"""
    if any(rel.split("/", 1)[0] == CONSOLE_CACHE for rel in title_files):
        return lambda rel: False
    return lambda rel: rel == CONSOLE_CACHE or rel.startswith(CONSOLE_CACHE + "/")


def verify(console, out, nup=None, place=INTERNAL, nup_at=None):
    """At the place sent to: every file's size there, and eboot.bin's CRC32 read back whole: [] when all is as on the PC,
    else what differs"""
    bad = []
    remote_title, remote_nup = place_paths(place)
    nup_at = nup_at if nup_at is not None or not nup else remote_nup
    sent = {REMOTE_DATA: {rel: os.path.getsize(f) for rel, f in data_files(out, nup_at).items()},
            remote_title: local_tree(os.path.join(out, "homebrew", TITLE_ID))}
    for remote_top, files in sent.items():
        there = console.tree(remote_top)
        for rel, size in files.items():
            if there.get(rel) != size:
                bad.append("%s/%s: %s bytes there, %d here" % (remote_top, rel, there.get(rel), size))
    if nup and console.size(remote_nup) != os.path.getsize(nup):
        bad.append("%s: %s bytes there, %d here" % (remote_nup, console.size(remote_nup), os.path.getsize(nup)))
    eboot = os.path.join(out, "homebrew", TITLE_ID, "eboot.bin")
    mine, theirs = crc32_file(eboot), console.crc32(remote_title + "/eboot.bin")
    if mine != theirs:
        bad.append("eboot.bin: CRC32 %d there, %d here" % (theirs, mine))
    return bad, mine


def remove_title(console, remote_title, out=None, host=None):
    """A title folder left at the other place (asked first), its files, then its folders the deepest first, then itself;
    only the title's own folder at a place, never another (WRITABLE, and the place's own path checked here)"""
    if remote_title not in [place_paths(q)[0] for q in PLACES]:
        raise SendError("refused: %s is not a place's title folder" % remote_title)
    journal = Journal(out, host or "console") if out else None
    for rel in sorted(console.tree(remote_title)):
        console.delete(remote_title + "/" + rel)
        if journal:
            journal.files.pop(remote_title + "/" + rel, None)
    for rel in sorted(console.folders(remote_title), key=lambda r: (-r.count("/"), r)):
        console.rmdir(remote_title + "/" + rel)
    console.rmdir(remote_title)
    if journal:
        journal.save()


def remove_nup(console, remote_nup):
    """A game file left at the other place (asked first, once the new one was read back)"""
    if remote_nup not in [place_paths(q)[1] for q in PLACES]:
        raise SendError("refused: %s is not a place's game file" % remote_nup)
    console.delete(remote_nup)


def main():
    ap = argparse.ArgumentParser(description="Grapnel: the title sent to the console over FTP")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=FTP_PORT)
    ap.add_argument("--out", help="the builder's output folder")
    ap.add_argument("--nup", help="the game file, sent as the place's game.nup when it is not there with its size")
    ap.add_argument("--place", default=INTERNAL, choices=PLACES, help="where the title and the game file go (default: internal)")
    ap.add_argument("--flat-dir", help="where the flat copy of a .nup whose game files are compressed is (default: where the build "
                    "put it)")
    ap.add_argument("--root", default="", help="(tests) a local folder standing for the console's /")
    ap.add_argument("--probe", action="store_true")
    a = ap.parse_args()
    if a.probe:
        print(probe(a.host))
        return 0
    if a.nup:   # (a .nup whose game files are compressed: its flat copy, which the console reads)
        a.nup, note = console_game_file(a.nup, a.out, a.flat_dir)
        if note:
            print(note)
    c = Console(a.host, a.port, a.root)
    try:
        p = plan(c, a.out, a.nup, a.place)
        print("plan:", p)
        print("saves:", backup_saves(c, a.out, p["saves"]))
        send(c, a.out, a.nup if p["nup"] and p["nup"]["send"] else None,
             report=lambda d, t, r, l, cur: None, host=a.host, place=a.place, nup_at=p["nup_at"])
        bad, crc = verify(c, a.out, a.nup, a.place, p["nup_at"])
        print("verify:", bad or "all there", "eboot CRC32 %d" % crc)
        return 1 if bad else 0
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
