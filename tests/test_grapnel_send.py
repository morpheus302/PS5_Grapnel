#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The assistant's step 6, grapnel_send.py, against tests/ftp_fake.py (ftpsrv's ways on a folder of this PC): a first send,
a send over an install with saves (backed up first, kept), an interrupted send gone on with, the game file only when it is
not there with its size, and nothing written outside the two folders."""
import os
import sys
import tempfile
import threading
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts", "grapnel-builder"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ftp_fake  # noqa: E402
import grapnel_send as gs  # noqa: E402


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


class Send(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.jail = os.path.realpath(os.path.join(t, "console"))
        os.makedirs(os.path.join(self.jail, "data"))
        self.out = os.path.join(t, "out")
        title = os.path.join(self.out, "homebrew", "GRPN00001")
        write(os.path.join(title, "eboot.bin"), os.urandom(3 << 20))
        write(os.path.join(title, "sce_sys", "param.json"), b'{"titleId": "GRPN00001"}')
        write(os.path.join(title, "radv-shader-cache", "mesa_shader_cache_db", "part0", "mesa_cache.db"), os.urandom(5 << 20))
        write(os.path.join(self.out, "data", "grapnel", "hookshot_env.txt"), b"A=1\n")
        write(os.path.join(self.out, "data", "grapnel", "image.bin"), os.urandom(2 << 20))
        self.nup = os.path.join(t, "game.nup")
        write(self.nup, os.urandom(4 << 20))
        self.server = ftp_fake.Server(self.jail)
        self.port = self.server.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def console(self):
        return gs.Console("127.0.0.1", self.port, root=self.jail)

    def there(self, *parts):
        return os.path.join(self.jail, *parts)

    def test_first_send(self):
        c = self.console()
        p = gs.plan(c, self.out, self.nup)
        self.assertEqual((p["replace"], p["saves"], p["nup"]["send"]), ([], [], True))
        self.assertIsNone(gs.backup_saves(c, self.out, p["saves"]))
        reports = []
        sent = gs.send(c, self.out, self.nup, report=lambda *a: reports.append(a), log=lambda *a: None)
        self.assertEqual(len(sent), 6)
        self.assertTrue(reports and reports[-1][0] == reports[-1][1])   # (all the bytes counted)
        self.assertEqual(sent[-1], "/data/homebrew/GRPN00001/eboot.bin")   # (the title last, its eboot at its very end)
        bad, crc = gs.verify(c, self.out, self.nup)
        self.assertEqual(bad, [])
        self.assertEqual(crc, zlib.crc32(read(os.path.join(self.out, "homebrew", "GRPN00001", "eboot.bin"))))
        self.assertEqual(read(self.there("data", "grapnel", "game.nup")), read(self.nup))
        self.assertEqual(sorted(os.listdir(self.there("data"))), ["grapnel", "homebrew"])   # (nothing else made)
        c.close()

    def test_over_an_install_with_saves(self):
        write(self.there("data", "grapnel", "user", "save0.bin"), b"my save")
        write(self.there("data", "hookshot", "user", "old.bin"), b"older save")
        write(self.there("data", "homebrew", "GRPN00001", "stale.txt"), b"from an older build")
        write(self.there("data", "homebrew", "OTHER0001", "eboot.bin"), b"another title")
        c = self.console()
        p = gs.plan(c, self.out)
        self.assertEqual(p["replace"], ["/data/homebrew/GRPN00001", "/data/grapnel"])
        self.assertEqual(p["saves"], ["/data/grapnel/user", "/data/hookshot/user"])
        dest, count = gs.backup_saves(c, self.out, p["saves"])
        self.assertEqual(count, 2)
        self.assertEqual(read(os.path.join(dest, "data", "grapnel", "user", "save0.bin")), b"my save")
        self.assertEqual(read(os.path.join(dest, "data", "hookshot", "user", "old.bin")), b"older save")
        gs.send(c, self.out, None, log=lambda *a: None)
        self.assertEqual(gs.verify(c, self.out)[0], [])
        self.assertEqual(read(self.there("data", "grapnel", "user", "save0.bin")), b"my save")   # (kept)
        self.assertFalse(os.path.exists(self.there("data", "homebrew", "GRPN00001", "stale.txt")))   # (the title's own)
        self.assertTrue(os.path.exists(self.there("data", "homebrew", "OTHER0001", "eboot.bin")))   # (untouched)
        self.assertTrue(os.path.exists(self.there("data", "hookshot", "user", "old.bin")))
        c.close()

    def test_an_interrupted_send_goes_on(self):
        self.server.cut_after = (3 << 20) + 12345   # (in image.bin's or the cache's middle, whichever comes then)
        c = self.console()
        with self.assertRaises(Exception):
            gs.send(c, self.out, self.nup, log=lambda *a: None)
        c.close()
        c = self.console()
        counted = []
        gs.send(c, self.out, self.nup, report=lambda done, total, *a: counted.append(total), log=lambda *a: None)
        self.assertLess(counted[-1], (2 + 4 + 3 + 5) << 20)   # (what was there kept, the cut file gone on with)
        self.assertEqual(gs.verify(c, self.out, self.nup)[0], [])
        self.assertTrue(any(l.startswith("REST ") and l != "REST 0" for l in self.server.log))
        c.close()

    def test_the_titles_old_folders_go(self):
        """The title's folders the new build does not have go too, the deepest first, the empty ones as well (a real console
        kept part3, part4 and part30 to part49 of an older cache empty, 24/11/2026); the build's own folders stay"""
        db = ("data", "homebrew", "GRPN00001", "radv-shader-cache", "mesa_shader_cache_db")
        write(self.there(*db, "part30", "mesa_cache.db"), b"older")
        os.makedirs(self.there(*db, "part31"))
        write(self.there("data", "homebrew", "GRPN00001", "old", "deep", "x.bin"), b"x")
        c = self.console()
        gs.send(c, self.out, None, log=lambda *a: None)
        self.assertEqual(sorted(os.listdir(self.there(*db))), ["part0"])
        self.assertFalse(os.path.exists(self.there("data", "homebrew", "GRPN00001", "old")))
        self.assertEqual(gs.verify(c, self.out)[0], [])
        c.close()

    def test_a_new_eboot_of_the_same_size_is_sent(self):
        """A new build's eboot.bin is as long as the last one's: the journal's CRC32 sends it, the size alone kept the old"""
        c = self.console()
        gs.send(c, self.out, None, log=lambda *a: None)
        eboot = os.path.join(self.out, "homebrew", "GRPN00001", "eboot.bin")
        write(eboot, os.urandom(3 << 20))
        sent = gs.send(c, self.out, None, log=lambda *a: None)
        self.assertEqual(sent, ["/data/homebrew/GRPN00001/eboot.bin"])   # (only it: the rest is known there)
        self.assertEqual(gs.verify(c, self.out)[0], [])
        os.remove(os.path.join(self.out, "console-sent.json"))   # (no journal, another PC's send: all again, whole)
        self.assertEqual(len(gs.send(c, self.out, None, log=lambda *a: None)), 5)
        c.close()

    def test_the_game_file_only_when_needed(self):
        c = self.console()
        gs.send(c, self.out, self.nup, log=lambda *a: None)
        self.assertFalse(gs.plan(c, self.out, self.nup)["nup"]["send"])
        write(self.nup, os.urandom(5 << 20))   # (another size: sent again)
        self.assertTrue(gs.plan(c, self.out, self.nup)["nup"]["send"])
        c.close()

    def test_nothing_outside_the_two_folders(self):
        c = self.console()
        for p in ("/data/other.bin", "/data/homebrew/OTHER0001/eboot.bin", "/data/grapnelx/a", "/system/x"):
            with self.assertRaises(gs.SendError):
                c.upload(self.nup, p)
            with self.assertRaises(gs.SendError):
                c.delete(p)
        c.close()

    def test_cancel(self):
        c = self.console()
        stop = threading.Event()
        stop.set()
        with self.assertRaises(gs.Cancelled):
            gs.send(c, self.out, self.nup, cancel=stop, log=lambda *a: None)
        c.close()


if __name__ == "__main__":
    unittest.main()
