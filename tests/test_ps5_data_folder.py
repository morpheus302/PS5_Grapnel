"""(ps5, lot 80) The game's data folder on the console: /data/grapnel, the port's public name, and /data/hookshot, the folder
before, taken over (host/posix/ps5_data_folder.h, used by host/posix/ps5_platform.c hos_ps5_data_init). No game, GPU or
console:

- ps5_data_folder.h compiled with a C compiler and run in a temporary folder standing for /data, with the real stat, lstat,
  rename and mkdir: a new console (neither folder: /data/grapnel made), the takeover (/data/hookshot renamed whole: its files,
  their bytes and its sub-folders under the new name, the old name gone), the fallback (a rename that fails on purpose, as on
  a mount point: the old folder used in place, nothing moved, no new folder made), both there (/data/grapnel used, the old
  one left as it was), a link of the old name, a file of the new name (never renamed over), no /data at all; the line said in
  each case; and a setting naming a file under the other folder read under the data folder (ps5_data_path_follow);
- the storage broker (host/posix/ps5_broker_payload.c) compiled for this PC, its /data and its port moved to the test's, and
  run against the broker of lot 79 (git show 371fbd06, the version 1 that served /data/hookshot alone and outlives its title):
  the old one started, its folder taken over by a rename, then the new one started: it asks the old one to exit with the
  token the takeover brought along, takes the port, answers a version 2 hello and writes under /data/grapnel, refuses a path
  outside, and makes no /data/hookshot again. And alone over a /data/hookshot used in place: it writes its token there and
  serves it, and makes no /data/grapnel beside it (the next start would take that empty folder for the data folder);
- by reading the sources: hos_ps5_data_init takes its folder from ps5_data_folder_choose, the trail opens the new folder
  first, main starts the broker for either folder and says the folder again after the settings (the log to the PC starts
  there), the settings' paths follow, no code names /data/hookshot by itself any more, the title id is GRPN00001.

    python -I -B tests/test_ps5_data_folder.py [--cc gcc]
"""
import argparse
import errno
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "POSIX folders, rename and sockets, and the broker built for Linux"}

ROOT = Path(__file__).resolve().parents[1]
BASE = "371fbd06"   # (lot 79: the last eboot whose broker is version 1 and whose data folder is /data/hookshot)

FOLDER_TEST = r'''#include "ps5_data_folder.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static int fail_rename;   /* (errno of the rename on purpose; 0: the real one) */
static int renames, mkdirs;
static int t_is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int t_exists(const char *p) { struct stat st; return !lstat(p, &st); }
static int t_rename(const char *a, const char *b) {
    renames++;
    if (fail_rename) { errno = fail_rename; return -1; }
    return rename(a, b);
}
static int t_mkdir(const char *p) { mkdirs++; if (mkdir(p, 0777)) return -1; chmod(p, 0777); return 0; }
static const struct ps5_data_ops ops = {t_is_dir, t_exists, t_rename, t_mkdir};
static char base[512];
static void at(char *out, const char *case_name, const char *rest) { snprintf(out, 600, "%s/%s%s", base, case_name, rest); }
static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); CHECK(f); fputs(text, f); fclose(f);
}
static int same(const char *path, const char *text) {
    char b[256] = {0};
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    const size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
    return n == strlen(text) && !memcmp(b, text, n);
}
static enum ps5_data_how choose(const char *parent, char *root, char *line, int *err) {
    return ps5_data_folder_choose(parent, &ops, root, 128, line, 400, err);
}
int main(int argc, char **argv) {
    (void)argc;
    snprintf(base, sizeof base, "%s", argv[1]);
    char p[600], q[600], root[128], line[400];
    int err;
    /* (1) a new console: neither folder, /data/grapnel made */
    at(p, "new", ""); CHECK(!mkdir(p, 0777));
    renames = mkdirs = 0;
    CHECK(choose(p, root, line, &err) == PS5_DATA_MADE && err == 0 && renames == 0 && mkdirs == 1);
    at(q, "new", "/grapnel"); CHECK(!strcmp(root, q) && t_is_dir(q));
    at(q, "new", "/hookshot"); CHECK(!t_exists(q));
    CHECK(strstr(line, "made (new") != NULL);
    /* ... and the next start finds it there */
    CHECK(choose(p, root, line, &err) == PS5_DATA_THERE && strstr(line, "there already") && !strstr(line, "too"));
    /* (2) the takeover: the old folder renamed whole, files, bytes and sub-folders, the old name gone */
    at(p, "takeover", ""); CHECK(!mkdir(p, 0777));
    at(q, "takeover", "/hookshot"); CHECK(!mkdir(q, 0777));
    at(q, "takeover", "/hookshot/user"); CHECK(!mkdir(q, 0777));
    at(q, "takeover", "/hookshot/user/save"); CHECK(!mkdir(q, 0777));
    at(q, "takeover", "/hookshot/user/save/progress.sav"); put(q, "the save");
    at(q, "takeover", "/hookshot/hookshot_env.txt"); put(q, "RX_VK_SKIP_FILE=/data/hookshot/skip.txt\n");
    at(q, "takeover", "/hookshot/hookshot_options.ini"); put(q, "fps_counter=1\n");
    at(q, "takeover", "/hookshot/hookshot_menu_fond.rgba"); put(q, "HKBG");
    renames = mkdirs = 0;
    CHECK(choose(p, root, line, &err) == PS5_DATA_TAKEN_OVER && err == 0 && renames == 1 && mkdirs == 0);
    at(q, "takeover", "/grapnel"); CHECK(!strcmp(root, q));
    at(q, "takeover", "/hookshot"); CHECK(!t_exists(q));
    at(q, "takeover", "/grapnel/user/save/progress.sav"); CHECK(same(q, "the save"));
    at(q, "takeover", "/grapnel/hookshot_env.txt"); CHECK(same(q, "RX_VK_SKIP_FILE=/data/hookshot/skip.txt\n"));
    at(q, "takeover", "/grapnel/hookshot_options.ini"); CHECK(same(q, "fps_counter=1\n"));
    at(q, "takeover", "/grapnel/hookshot_menu_fond.rgba"); CHECK(same(q, "HKBG"));
    CHECK(strstr(line, "taken over: renamed to") != NULL && strstr(line, "nothing copied") != NULL);
    /* ... and the next start: there, nothing renamed again */
    renames = 0;
    CHECK(choose(p, root, line, &err) == PS5_DATA_THERE && renames == 0);
    /* (3) the fallback: the rename refused (a mount point: EXDEV; the sandbox: EPERM): the old folder in place, as it is */
    at(p, "fallback", ""); CHECK(!mkdir(p, 0777));
    at(q, "fallback", "/hookshot"); CHECK(!mkdir(q, 0777));
    at(q, "fallback", "/hookshot/hookshot_options.ini"); put(q, "kept\n");
    fail_rename = EXDEV, renames = mkdirs = 0;
    CHECK(choose(p, root, line, &err) == PS5_DATA_IN_PLACE && err == EXDEV && renames == 1 && mkdirs == 0);
    at(q, "fallback", "/hookshot"); CHECK(!strcmp(root, q));
    at(q, "fallback", "/grapnel"); CHECK(!t_exists(q));   /* (never made beside it: the next start would take it) */
    at(q, "fallback", "/hookshot/hookshot_options.ini"); CHECK(same(q, "kept\n"));
    CHECK(strstr(line, "used in place: renaming it to") && strstr(line, "errno 18") && strstr(line, "next start tries again"));
    /* ... the next start tries again, and with the rename allowed the folder is taken over then */
    fail_rename = EPERM;
    CHECK(choose(p, root, line, &err) == PS5_DATA_IN_PLACE && err == EPERM);
    fail_rename = 0;
    CHECK(choose(p, root, line, &err) == PS5_DATA_TAKEN_OVER);
    at(q, "fallback", "/grapnel/hookshot_options.ini"); CHECK(same(q, "kept\n"));
    /* (4) both there: the new one used, the old one left as it was, not read */
    at(p, "both", ""); CHECK(!mkdir(p, 0777));
    at(q, "both", "/grapnel"); CHECK(!mkdir(q, 0777));
    at(q, "both", "/hookshot"); CHECK(!mkdir(q, 0777));
    at(q, "both", "/hookshot/old.txt"); put(q, "old");
    renames = mkdirs = 0;
    CHECK(choose(p, root, line, &err) == PS5_DATA_THERE && renames == 0 && mkdirs == 0);
    at(q, "both", "/grapnel"); CHECK(!strcmp(root, q));
    at(q, "both", "/hookshot/old.txt"); CHECK(same(q, "old"));
    CHECK(strstr(line, "is there too: left as it is") != NULL);
    /* (5) a link of the old name to a folder elsewhere (another disk): the link renamed, the folder behind it the data */
    at(p, "link", ""); CHECK(!mkdir(p, 0777));
    at(q, "link", "/elsewhere"); CHECK(!mkdir(q, 0777));
    at(q, "link", "/elsewhere/game.nup"); put(q, "nup");
    char link_to[600]; at(link_to, "link", "/elsewhere");
    at(q, "link", "/hookshot"); CHECK(!symlink(link_to, q));
    CHECK(choose(p, root, line, &err) == PS5_DATA_TAKEN_OVER);
    at(q, "link", "/grapnel/game.nup"); CHECK(same(q, "nup"));
    /* (6) a file of the new name: never renamed over nor removed */
    at(p, "file", ""); CHECK(!mkdir(p, 0777));
    at(q, "file", "/grapnel"); put(q, "not a folder");
    at(q, "file", "/hookshot"); CHECK(!mkdir(q, 0777));
    renames = 0;
    CHECK(choose(p, root, line, &err) == PS5_DATA_IN_PLACE && renames == 0);
    at(q, "file", "/hookshot"); CHECK(!strcmp(root, q));
    at(q, "file", "/grapnel"); CHECK(same(q, "not a folder"));
    at(q, "file", "/hookshot"); CHECK(!rmdir(q));
    CHECK(choose(p, root, line, &err) == PS5_DATA_NONE && err == EEXIST && !root[0]);
    /* (7) no /data at all: none, mkdir's errno (the caller asks the Lapy daemon or uses /app0) */
    at(p, "missing/parent", "");
    CHECK(choose(p, root, line, &err) == PS5_DATA_NONE && err == ENOENT && !root[0] && strstr(line, "could not be made"));
    /* (8) a setting's file under the other folder, read under the data folder */
    char out[256];
    CHECK(ps5_data_path_follow("/data/hookshot/skip.txt", "/data/grapnel", out, sizeof out) && !strcmp(out, "/data/grapnel/skip.txt"));
    CHECK(ps5_data_path_follow("/data/grapnel/skip.txt", "/data/hookshot", out, sizeof out) && !strcmp(out, "/data/hookshot/skip.txt"));
    CHECK(ps5_data_path_follow("/data/hookshot/a/b.txt", "/data/grapnel", out, sizeof out) && !strcmp(out, "/data/grapnel/a/b.txt"));
    CHECK(!ps5_data_path_follow("/data/grapnel/skip.txt", "/data/grapnel", out, sizeof out));   /* (its own folder) */
    CHECK(!ps5_data_path_follow("/data/hookshot/skip.txt", "/app0", out, sizeof out));          /* (no data folder) */
    CHECK(!ps5_data_path_follow("/data/hookshotx/skip.txt", "/data/grapnel", out, sizeof out)); /* (another folder) */
    CHECK(!ps5_data_path_follow("/data/hookshot", "/data/grapnel", out, sizeof out));           /* (the folder, no file) */
    CHECK(!ps5_data_path_follow("/data/hookshot/", "/data/grapnel", out, sizeof out));
    CHECK(!ps5_data_path_follow("2560x1440", "/data/grapnel", out, sizeof out));
    CHECK(!ps5_data_path_follow("/data/hookshot/skip.txt", "/data/grapnel", out, 10));            /* (too long: as it is) */
    CHECK(!strcmp(ps5_data_how_name(PS5_DATA_TAKEN_OVER), "taken over") && !strcmp(ps5_data_how_name(PS5_DATA_IN_PLACE), "in place"));
    CHECK(!strcmp(PS5_DATA_DIR, "/data/grapnel") && !strcmp(PS5_DATA_OLD_DIR, "/data/hookshot"));
    puts("PASS: the data folder: made, taken over whole, used in place when the rename fails, left alone, paths followed");
    return 0;
}
'''

HB_MAGIC = 0x314b4248
HB_WRITE, HB_READ, HB_CREATE, HB_MKDIR = 0x45545257, 0x44414552, 0x54414552, 0x52444b4d


def need(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def listening(port, seconds=6.0):
    end = time.time() + seconds
    while time.time() < end:
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.05)
    return False


class Client:
    """The title's side of the protocol (host/posix/ps5_broker_client.c), in Python"""

    def __init__(self, port, token, version):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.sendall(struct.pack("<II16s", HB_MAGIC, version, token))
        self.hello = self._reply()

    def _recv(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError("closed")
            b += c
        return b

    def _reply(self):
        magic, result = struct.unpack("<Ii", self._recv(8))
        need(magic == HB_MAGIC, "the broker's reply has another magic")
        return result

    def request(self, magic, path, offset=0, data=b"", length=None, reserved=0):
        p = path.encode()
        self.s.sendall(struct.pack("<IIQII", magic, len(p), offset, len(data) if length is None else length, reserved) + p + data)
        r = self._reply()
        if magic == HB_READ and r > 0:
            return r, self._recv(r)
        return r, b""

    def close(self):
        self.s.close()


def token_of(folder):
    return (Path(folder) / ".hookshot-broker-token").read_bytes()


def build_broker(cc, src_dir, out, parent, port, fake):
    """The broker of src_dir (host/posix there), for this PC: its /data the test's folder, its port the test's"""
    cmd = [cc, "-O1", "-Wall", "-Wextra", "-pthread", "-include", "sys/vfs.h", "-I", str(src_dir), "-I", str(fake),
           '-DPS5_DATA_PARENT="%s"' % parent, "-DHB_PORT=%d" % port, "-o", str(out), str(src_dir / "ps5_broker_payload.c")]
    subprocess.run(cmd, check=True)


def broker_check(cc, tmp):
    fake = tmp / "fake"
    (fake / "ps5").mkdir(parents=True)
    (fake / "ps5" / "klog.h").write_text('#include <stdio.h>\n#define klog_printf(...) (fprintf(stderr, __VA_ARGS__), fflush(stderr))\n')
    # ---- lot 79's broker, version 1: its sources from git, /data/hookshot and the port moved to the test's ----
    try:
        old_c = subprocess.run(["git", "-C", str(ROOT), "show", BASE + ":host/posix/ps5_broker_payload.c"], check=True,
                               capture_output=True, text=True).stdout
        old_h = subprocess.run(["git", "-C", str(ROOT), "show", BASE + ":host/posix/ps5_broker.h"], check=True,
                               capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        print("skip: the broker against lot 79's: %s is not in this repository's history" % BASE)
        return
    data = tmp / "data"
    data.mkdir()
    port = free_port()
    old_dir = tmp / "old"
    old_dir.mkdir()
    need("#define HB_VERSION 1" in old_h and '#define HB_ROOT "/data/hookshot/"' in old_h, "lot 79's broker header is not version 1")
    old_h = old_h.replace("#define HB_PORT 9147", "#define HB_PORT %d" % port).replace('"/data/hookshot/"', '"%s/hookshot/"' % data)
    old_c = old_c.replace('"/data/hookshot"', '"%s/hookshot"' % data).replace('open("/data",', 'open("%s",' % data)
    (old_dir / "ps5_broker.h").write_text(old_h)
    (old_dir / "ps5_broker_payload.c").write_text(old_c)
    subprocess.run([cc, "-O1", "-pthread", "-include", "sys/vfs.h", "-I", str(old_dir), "-I", str(fake), "-o", str(tmp / "broker-v1"),
                    str(old_dir / "ps5_broker_payload.c")], check=True)
    build_broker(cc, ROOT / "host" / "posix", tmp / "broker-v2", data, port, fake)
    procs = []
    try:
        # (lot 79's title made its folder and started its broker, which outlives it)
        (data / "hookshot").mkdir()
        (data / "hookshot" / "user").mkdir()
        (data / "hookshot" / "user" / "progress.sav").write_bytes(b"save")
        v1 = subprocess.Popen([str(tmp / "broker-v1")], stderr=subprocess.PIPE, text=True)
        procs.append(v1)
        need(listening(port), "lot 79's broker does not listen")
        old_token = token_of(data / "hookshot")
        c = Client(port, old_token, 1)
        need(c.hello == 0, "lot 79's broker refuses its own token")
        c.close()
        # (the first Grapnel start: the takeover renames the folder, the old broker's token file with it)
        os.rename(data / "hookshot", data / "grapnel")
        # (its title's hello: version 2 with the token it reads, the old one's: answered -EPROTONOSUPPORT, so it starts its own)
        c = Client(port, token_of(data / "grapnel"), 2)
        need(c.hello == -errno.EPROTONOSUPPORT, "lot 79's broker did not answer a version 2 hello -EPROTONOSUPPORT (%d)" % c.hello)
        c.close()
        v2 = subprocess.Popen([str(tmp / "broker-v2")], stderr=subprocess.PIPE, text=True)
        procs.append(v2)
        try:
            v1.wait(timeout=8)
        except subprocess.TimeoutExpired:
            raise SystemExit("FAIL: the new broker did not get lot 79's to exit (the takeover took its token file along)")
        need(v1.returncode == 0, "lot 79's broker did not exit cleanly (%s)" % v1.returncode)
        old_said = v1.stderr.read()
        need("replaced by a new broker; exiting" in old_said, "lot 79's broker did not say it was replaced: " + old_said[-300:])
        need(listening(port), "the new broker does not listen once the old one left")
        time.sleep(0.3)
        need(v2.poll() is None, "the new broker stopped")
        new_token = token_of(data / "grapnel")
        need(new_token != old_token, "the new broker kept the old token")
        need(not (data / "hookshot").exists(), "the new broker made /data/hookshot again")
        c = Client(port, new_token, 2)
        need(c.hello == 0, "the new broker refuses a version 2 hello with its token (%d)" % c.hello)
        target = "%s/grapnel/cache/big.bin" % data
        need(c.request(HB_MKDIR, "%s/grapnel/cache" % data)[0] == 0, "no folder made under /data/grapnel")
        need(c.request(HB_CREATE, target, reserved=1)[0] == 0, "no file made under /data/grapnel")
        blob = bytes(range(256)) * 64
        need(c.request(HB_WRITE, target, 0, blob)[0] == len(blob), "a write under /data/grapnel refused")
        r, got = c.request(HB_READ, target, 0, length=len(blob))
        need(r == len(blob) and got == blob, "what was written under /data/grapnel does not read back")
        need(c.request(HB_CREATE, "%s/elsewhere.bin" % data)[0] == -errno.EACCES, "a path outside the data folders is not refused")
        need(c.request(HB_CREATE, "%s/grapnel/../x.bin" % data)[0] == -errno.EACCES, "a path climbing out is not refused")
        need(c.request(HB_READ, "%s/grapnel/user/progress.sav" % data, 0, length=16)[1] == b"save", "the save does not read back")
        c.close()
        # (the old token no longer opens it)
        try:
            c = Client(port, old_token, 2)
            refused = False
        except (ConnectionError, OSError):
            refused = True
        need(refused, "the new broker still takes the old token")
        v2.terminate()
        v2.wait(timeout=5)
        new_said = v2.stderr.read()
        need("asked the running broker to exit" in new_said and "an earlier token" in new_said,
             "the new broker did not say it asked the old one to exit with the earlier token: " + new_said[-400:])
        # ---- alone over a /data/hookshot used in place (its rename failed): served, and no /data/grapnel made beside it ----
        data2 = tmp / "data2"
        data2.mkdir()
        port2 = free_port()
        build_broker(cc, ROOT / "host" / "posix", tmp / "broker-v2b", data2, port2, fake)
        (data2 / "hookshot").mkdir()
        v2b = subprocess.Popen([str(tmp / "broker-v2b")], stderr=subprocess.PIPE, text=True)
        procs.append(v2b)
        need(listening(port2), "the new broker does not listen over /data/hookshot alone")
        need(not (data2 / "grapnel").exists(), "the new broker made /data/grapnel beside a /data/hookshot used in place")
        c = Client(port2, token_of(data2 / "hookshot"), 2)
        need(c.hello == 0, "the new broker's token in /data/hookshot is refused")
        target = "%s/hookshot/hookshot_programs.bin" % data2
        need(c.request(HB_CREATE, target, reserved=1)[0] == 0 and c.request(HB_WRITE, target, 0, b"x" * 4096)[0] == 4096,
             "no write under a /data/hookshot used in place")
        c.close()
        # ---- and on a new console (no folder at all): /data/grapnel made, never /data/hookshot ----
        data3 = tmp / "data3"
        data3.mkdir()
        port3 = free_port()
        build_broker(cc, ROOT / "host" / "posix", tmp / "broker-v2c", data3, port3, fake)
        v2c = subprocess.Popen([str(tmp / "broker-v2c")], stderr=subprocess.PIPE, text=True)
        procs.append(v2c)
        need(listening(port3), "the new broker does not listen on a new console")
        need((data3 / "grapnel" / ".hookshot-broker-token").exists() and not (data3 / "hookshot").exists(),
             "on a new console the token is not in a /data/grapnel it made (or /data/hookshot was made)")
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
    print("ok: the broker of lot 79 replaced after the takeover; version 2 serves /data/grapnel, or /data/hookshot in place")


def check_sources():
    platform = (ROOT / "host/posix/ps5_platform.c").read_text(encoding="utf-8")
    early = (ROOT / "host/posix/ps5_early.c").read_text(encoding="utf-8")
    main_c = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    broker_h = (ROOT / "host/posix/ps5_broker.h").read_text(encoding="utf-8")
    client = (ROOT / "host/posix/ps5_broker_client.c").read_text(encoding="utf-8")
    tool = (ROOT / "nx2/toolchain_ps5.py").read_text(encoding="utf-8")
    init = block(platform, "int hos_ps5_data_init(void) {")
    choose = block(platform, "static int data_choose(const char *how_prefix) {")
    need("ps5_data_folder_choose(PS5_DATA_PARENT, &data_ops," in choose, "hos_ps5_data_init does not choose by ps5_data_folder.h")
    need("hos_ps5_trail_renamed(PS5_DATA_OLD_DIR, PS5_DATA_DIR)" in choose, "the trail is not told of the rename")
    need('hos_ps5_trail("ps5: data folder: %s", data_status);' in choose, "the choice is not said in the trail")
    need(init.count("data_choose(") == 2, "the Lapy daemon's path does not choose the same way")
    need("static int data_rename(const char *from, const char *to) { return rename(from, to); }" in platform,
         "the console's rename is not the folder's rename")
    need("return e && *e ? e : data_granted ? data_root : \"/app0\";" in block(platform, "const char *hos_ps5_data_root(void) {"),
         "hos_ps5_data_root does not answer the chosen folder")
    # (no code names the old folder by itself any more: the names are ps5_data_folder.h's)
    for name, text in (("host/posix/ps5_platform.c", platform), ("host/posix/ps5_early.c", early), ("host/hos_main.c", main_c)):
        code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        need('"/data/hookshot' not in code, "%s names /data/hookshot in its code" % name)
    first = block(early, "static void early(const char *by) {")
    need(first.index("stat(PS5_DATA_DIR, &st)") < first.index("stat(PS5_DATA_OLD_DIR, &st)"),
         "the trail does not look for /data/grapnel before /data/hookshot")
    renamed = block(early, "void hos_ps5_trail_renamed(const char *from, const char *to) {")
    need("trail_lock();" in renamed and "snprintf(trail_path, sizeof trail_path" in renamed, "the trail's path does not follow the rename")
    # (main: the broker for either folder; the folder said again after the settings; the settings' paths followed)
    need("else if (!hb_serves_root(hos_ps5_data_root()))" in main_c, "stage 5 does not start the broker for either data folder")
    env = block(main_c, "static int load_longshot_env(void) {")
    need(re.search(r"#ifdef __PROSPERO__\s*\{[^#]*ps5_data_path_follow\(eq \+ 1, hos_ps5_data_root\(\), moved, sizeof moved\)", env),
         "the settings' paths do not follow the data folder (console only)")
    stage4 = main_c[main_c.index('"ps5: stage 4 settings: hookshot_env.txt: %d settings"'):]
    need(stage4.index('ps5_stage("ps5: stage 4 data folder: %s (the game folder %s)"') < stage4.index("stage 5 broker"),
         "the data folder is not said again after the settings")
    # (the broker: version 2, both folders, the token read from either, the new title folder)
    need("#define HB_VERSION 2" in broker_h and "#define HB_ROOT PS5_DATA_DIR \"/\"" in broker_h and
         "#define HB_ROOT_OLD PS5_DATA_OLD_DIR \"/\"" in broker_h, "the broker is not version 2 over both folders")
    need('"/data/homebrew/GRPN00001"' in broker_h, "the token is not copied into the Grapnel title's folder")
    need("{HB_ROOT HB_TOKEN_NAME, HB_ROOT_OLD HB_TOKEN_NAME, \"/app0/\" HB_TOKEN_NAME}" in client,
         "the title does not read the token from either folder")
    # (the title: its id, its name under the tile)
    need('TITLE_ID = "GRPN00001"' in tool and 'TITLE_NAME = "Grapnel"' in tool and 'CONTENT_LABEL = "GRAPNELPS5000000"' in tool,
         "the title is not GRPN00001 Grapnel")
    need(len("GRAPNELPS5000000") == 16, "the content label is not 16 characters")
    print("ok: the sources choose the folder by ps5_data_folder.h, the trail, the broker and the settings follow it")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="ps5-data-folder-") as tmp:
        t = Path(tmp)
        (t / "folder.c").write_text(FOLDER_TEST, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host/posix"),
                        str(t / "folder.c"), "-o", str(t / "folder")], check=True)
        (t / "base").mkdir()
        r = subprocess.run([str(t / "folder"), str(t / "base")], capture_output=True, text=True)
        need(r.returncode == 0, "the data folder's cases: %s%s" % (r.stdout, r.stderr))
        print(r.stdout.strip())
        broker_check(a.cc, t)
    return 0


if __name__ == "__main__":
    sys.exit(main())
