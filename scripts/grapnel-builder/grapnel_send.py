#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel: the title sent to the console (the assistant's step 6), over FTP.

The way: FTP, which every jailbroken PS5 setup can run. If an FTP server already answers on the console (ftpsrv's port 2121),
it is used; otherwise the builder sends ftpsrv (ps5-payload-dev/ftpsrv, GPL-3.0-or-later, built from its source with the
toolchain's payload SDK: toolchain/ftpsrv/) to the console's ELF loader (port 9021), as the title sends its broker. FTP needs
nothing more on the PC than Python's ftplib, and ftpsrv's own commands are known: USER without a password, STOR after REST
writes from that offset (an interrupted send goes on), SIZE, MLSD of the current folder, RETR, MKD, DELE, RMD, and SELF,
which turns its on-the-fly SELF decryption off so that a file comes back as it is.

What it touches on the console: /data/homebrew/GRPN00001 and /data/grapnel, nothing else (_check_write). Before anything is
replaced, the player's saves (/data/grapnel/user, or /data/hookshot/user for an older install) are copied to the PC, and the
send stops if that fails. What the console already has is known from a journal on the PC (<out>/console-sent.json, per
console address): each file sent whole, with its size and CRC32. A file is kept when the journal has it with the size and CRC32
of the PC's file and the console has it with that size (a new eboot.bin is as long as the last one: the size alone would keep
the old); a file the journal has as begun with this content and the console has shorter is gone on with from its end; any
other is sent whole. The title's files and folders the new build does not have are removed (in /data/homebrew/GRPN00001 only:
/data/grapnel keeps the saves and the game file). The .nup goes to /data/grapnel/game.nup only when it is not there with the
same size (the builder checked it is the game; its 26 GB are not read for a CRC). The title folder goes
last, eboot.bin at its very end, so that ShadowMountPlus (which registers what it finds in /data/homebrew once it has stopped
changing) never sees half a title.

    python3 grapnel_send.py --host <console address> --out <output folder> [--nup F] [--probe] [--root R] [--port N]

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
REMOTE_TITLE = "/data/homebrew/" + TITLE_ID
REMOTE_DATA = "/data/grapnel"
REMOTE_NUP = REMOTE_DATA + "/game.nup"
REMOTE_SAVES = ("/data/grapnel/user", "/data/hookshot/user")
WRITABLE = (REMOTE_TITLE, REMOTE_DATA)
BLOCK = 1 << 20


class SendError(Exception):
    pass


class Cancelled(SendError):
    pass


def probe(host, timeout=3.0):
    """What answers on the console: {"ftp": the FTP server's greeting or None, "elfldr": the ELF loader's port is open}"""
    found = {"ftp": None, "elfldr": False}
    try:
        with socket.create_connection((host, FTP_PORT), timeout=timeout) as s:
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
        if not any(p == w or p.startswith(w + "/") for w in WRITABLE):
            raise SendError("refused: %s is outside %s" % (p, " and ".join(WRITABLE)))

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
        """p and the folders above it that are not there: only within the two folders, and /data/homebrew itself when a
        console has none yet (where every homebrew title goes)"""
        self._check_write(p)
        parts = p.strip("/").split("/")
        for i in range(1, len(parts) + 1):
            q = "/" + "/".join(parts[:i])
            if not self.isdir(q):
                if q != "/data/homebrew":
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


def plan(console, out, nup=None):
    """What a send would do: the title and data folders there, the saves there, the game file there or not"""
    p = {"title_there": console.isdir(REMOTE_TITLE), "data_there": console.isdir(REMOTE_DATA),
         "saves": [s for s in REMOTE_SAVES if console.isdir(s)], "nup": None}
    if nup:
        there = console.size(REMOTE_NUP)
        p["nup"] = {"local": os.path.getsize(nup), "there": there, "send": there != os.path.getsize(nup)}
    p["replace"] = [d for d, there in ((REMOTE_TITLE, p["title_there"]), (REMOTE_DATA, p["data_there"])) if there]
    return p


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


def send(console, out, nup=None, report=None, cancel=None, log=print, host=None):
    """The data folder, the game file when it is not there, then the title (eboot.bin last): kept what the journal and the
    console both have as on the PC, gone on with what was begun with this content, the rest sent whole; then the title's
    leftovers removed. Returns the files sent"""
    journal = Journal(out, host or "console")
    jobs = []
    title_local = os.path.join(out, "homebrew", TITLE_ID)
    data_local = os.path.join(out, "data", "grapnel")
    for local_top, remote_top in ((data_local, REMOTE_DATA), (title_local, REMOTE_TITLE)):
        if not os.path.isdir(local_top):
            raise SendError("%s is not there: build the game first" % local_top)
    data_files = local_tree(data_local)
    title_files = local_tree(title_local)
    there = {REMOTE_DATA: console.tree(REMOTE_DATA), REMOTE_TITLE: console.tree(REMOTE_TITLE)}
    for rel, size in data_files.items():
        jobs.append((os.path.join(data_local, *rel.split("/")), REMOTE_DATA + "/" + rel, size, there[REMOTE_DATA].get(rel)))
    if nup:
        jobs.append((nup, REMOTE_NUP, os.path.getsize(nup), there[REMOTE_DATA].get("game.nup")))
    order = sorted(title_files, key=lambda r: (r == "eboot.bin", r))
    for rel in order:
        jobs.append((os.path.join(title_local, *rel.split("/")), REMOTE_TITLE + "/" + rel, title_files[rel],
                     there[REMOTE_TITLE].get(rel)))
    plan_ = []
    for local, remote, size, t in jobs:
        if remote == REMOTE_NUP:   # (the game file by its size alone)
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
    # (the title's files the build no longer has: its folder is the title's alone)
    for rel in sorted(set(there[REMOTE_TITLE]) - set(title_files)):
        console.delete(REMOTE_TITLE + "/" + rel)
        journal.files.pop(REMOTE_TITLE + "/" + rel, None)
    # (and its folders the build does not have, emptied above or empty already: the deepest first)
    mine = {os.path.relpath(d, title_local).replace(os.sep, "/") for d, _, _ in os.walk(title_local)} - {"."}
    for rel in sorted(set(console.folders(REMOTE_TITLE)) - mine, key=lambda r: (-r.count("/"), r)):
        console.rmdir(REMOTE_TITLE + "/" + rel)
    journal.save()
    return [r for _, r, _, _, _ in todo]


def verify(console, out, nup=None):
    """Every file's size there, and eboot.bin's CRC32 read back whole: [] when all is as on the PC, else what differs"""
    bad = []
    for local_top, remote_top in ((os.path.join(out, "data", "grapnel"), REMOTE_DATA),
                                  (os.path.join(out, "homebrew", TITLE_ID), REMOTE_TITLE)):
        there = console.tree(remote_top)
        for rel, size in local_tree(local_top).items():
            if there.get(rel) != size:
                bad.append("%s/%s: %s bytes there, %d here" % (remote_top, rel, there.get(rel), size))
    if nup and console.size(REMOTE_NUP) != os.path.getsize(nup):
        bad.append("%s: %s bytes there, %d here" % (REMOTE_NUP, console.size(REMOTE_NUP), os.path.getsize(nup)))
    eboot = os.path.join(out, "homebrew", TITLE_ID, "eboot.bin")
    mine, theirs = crc32_file(eboot), console.crc32(REMOTE_TITLE + "/eboot.bin")
    if mine != theirs:
        bad.append("eboot.bin: CRC32 %d there, %d here" % (theirs, mine))
    return bad, mine


def main():
    ap = argparse.ArgumentParser(description="Grapnel: the title sent to the console over FTP")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=FTP_PORT)
    ap.add_argument("--out", help="the builder's output folder")
    ap.add_argument("--nup", help="the game file, sent as /data/grapnel/game.nup when it is not there with its size")
    ap.add_argument("--root", default="", help="(tests) a local folder standing for the console's /")
    ap.add_argument("--probe", action="store_true")
    a = ap.parse_args()
    if a.probe:
        print(probe(a.host))
        return 0
    c = Console(a.host, a.port, a.root)
    try:
        p = plan(c, a.out, a.nup)
        print("plan:", p)
        print("saves:", backup_saves(c, a.out, p["saves"]))
        send(c, a.out, a.nup if p["nup"] and p["nup"]["send"] else None,
             report=lambda d, t, r, l, cur: None, host=a.host)
        bad, crc = verify(c, a.out, a.nup)
        print("verify:", bad or "all there", "eboot CRC32 %d" % crc)
        return 1 if bad else 0
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
