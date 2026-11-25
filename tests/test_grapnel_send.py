#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The assistant's step 6, grapnel_send.py, against tests/ftp_fake.py (ftpsrv's ways on a folder of this PC): a first send,
a send over an install with saves (backed up first, kept), an interrupted send gone on with, the game file only when it is
not there with its size, and nothing written outside the two folders. And the extension disk (a /mnt/ext1 of the fake
console, with other titles in its homebrew folder): found, sent to, the title and the game file moved there from the
internal storage and back (the old ones removed only when asked), HOOKSHOT_PS5_NUP in the hookshot_env.txt sent when the
game file is there and not otherwise, the other titles never touched."""
import json
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

    def test_the_consoles_own_cache_stays(self):
        """(a PC without AVX2) a build with no driver's cache of its own: the console prepared the shaders itself into the
        title's radv-shader-cache (an hour), and a send of the build leaves it there; the title's other leftovers still go.
        A build with its own cache replaces it as before (test_the_titles_old_folders_go)"""
        import shutil
        shutil.rmtree(os.path.join(self.out, "homebrew", "GRPN00001", "radv-shader-cache"))
        db = ("data", "homebrew", "GRPN00001", "radv-shader-cache", "mesa_shader_cache_db")
        write(self.there(*db, "part0", "mesa_cache.db"), b"made by the console")
        write(self.there(*db, "part0", "mesa_cache.idx"), b"index")
        os.makedirs(self.there(*db, "part7"))
        write(self.there("data", "homebrew", "GRPN00001", "old", "x.bin"), b"x")
        write(self.there("data", "grapnel", "hookshot_prepare_progress.txt"), b"finished 1\n")
        c = self.console()
        gs.send(c, self.out, None, log=lambda *a: None)
        self.assertEqual(gs.verify(c, self.out)[0], [])
        self.assertEqual(read(self.there(*db, "part0", "mesa_cache.db")), b"made by the console")
        self.assertTrue(os.path.isdir(self.there(*db, "part7")))
        self.assertFalse(os.path.exists(self.there("data", "homebrew", "GRPN00001", "old")))   # (the rest goes, as before)
        self.assertEqual(read(self.there("data", "grapnel", "hookshot_prepare_progress.txt")), b"finished 1\n")
        self.assertFalse(gs.console_cache_kept(["eboot.bin", "radv-shader-cache/mesa_shader_cache_db/part0/mesa_cache.db"])(
            "radv-shader-cache/mesa_shader_cache_db/part9/mesa_cache.db"))
        self.assertFalse(gs.console_cache_kept(["eboot.bin"])("radv-shader-cachex/a"))
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

    # ---- the extension disk ----

    def ext1(self, other=True):
        """The fake console's extension disk: /mnt/ext1 with another title in its homebrew folder, and /mnt/ext0 a mount
        point with no disk (empty)"""
        os.makedirs(self.there("mnt", "ext0"))
        if other:
            write(self.there("mnt", "ext1", "homebrew", "OTHER0001", "eboot.bin"), b"another title")
        else:
            write(self.there("mnt", "ext1", "user", "x.bin"), b"the disk's own")

    def env_there(self):
        return read(self.there("data", "grapnel", "hookshot_env.txt")).decode()

    def test_the_extension_disk_is_found(self):
        c = self.console()
        self.assertEqual(gs.volumes(c), [])
        self.ext1()
        self.assertEqual(gs.volumes(c), ["/mnt/ext1"])   # (ext0 is an empty mount point: no disk)
        self.assertEqual(gs.where(c), {"title": [], "nup": {}})
        c.close()

    def test_send_to_the_extension_disk(self):
        self.ext1()
        c = self.console()
        p = gs.plan(c, self.out, self.nup, "/mnt/ext1")
        self.assertEqual((p["title"], p["nup_path"], p["nup_at"]), ("/mnt/ext1/homebrew/GRPN00001", "/mnt/ext1/grapnel/game.nup",
                                                                     "/mnt/ext1/grapnel/game.nup"))
        self.assertEqual((p["replace"], p["title_elsewhere"], p["nup_elsewhere"], p["nup"]["send"]), ([], [], [], True))
        sent = gs.send(c, self.out, self.nup, log=lambda *a: None, place="/mnt/ext1", nup_at=p["nup_at"])
        self.assertEqual(sent[-1], "/mnt/ext1/homebrew/GRPN00001/eboot.bin")
        bad, crc = gs.verify(c, self.out, self.nup, "/mnt/ext1", p["nup_at"])
        self.assertEqual(bad, [])
        self.assertEqual(read(self.there("mnt", "ext1", "grapnel", "game.nup")), read(self.nup))
        self.assertTrue(os.path.isfile(self.there("mnt", "ext1", "homebrew", "GRPN00001", "eboot.bin")))
        self.assertEqual(sorted(os.listdir(self.there("data"))), ["grapnel"])   # (nothing in /data/homebrew)
        self.assertFalse(os.path.exists(self.there("data", "grapnel", "game.nup")))
        self.assertEqual(read(self.there("mnt", "ext1", "homebrew", "OTHER0001", "eboot.bin")), b"another title")
        self.assertEqual(sorted(os.listdir(self.there("mnt", "ext1", "homebrew"))), ["GRPN00001", "OTHER0001"])
        self.assertEqual(sorted(os.listdir(self.there("mnt", "ext1"))), ["grapnel", "homebrew"])
        env = self.env_there()
        self.assertTrue(env.startswith("A=1\n"))
        self.assertIn("\nHOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup\n", env)
        self.assertEqual(read(os.path.join(self.out, "data", "grapnel", "hookshot_env.txt")), b"A=1\n")   # (the build's kept)
        # (a second send keeps all of it: the made hookshot_env.txt too)
        self.assertEqual(gs.send(c, self.out, None, log=lambda *a: None, place="/mnt/ext1", nup_at=p["nup_at"]), [])
        c.close()

    def test_the_internal_storage_has_no_nup_line(self):
        self.ext1()
        write(os.path.join(self.out, "data", "grapnel", "hookshot_env.txt"), b"A=1\nHOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup\n")
        c = self.console()
        p = gs.plan(c, self.out, self.nup)
        gs.send(c, self.out, self.nup, log=lambda *a: None, nup_at=p["nup_at"])
        self.assertEqual(gs.verify(c, self.out, self.nup, gs.INTERNAL, p["nup_at"])[0], [])
        self.assertEqual(self.env_there(), "A=1\n")   # (no line, even one a hand put in the build's file)
        write(os.path.join(self.out, "data", "grapnel", "hookshot_env.txt"), b"A=1\r\nB=2\r\n")
        gs.send(c, self.out, None, log=lambda *a: None, nup_at=p["nup_at"])
        self.assertEqual(read(self.there("data", "grapnel", "hookshot_env.txt")), b"A=1\r\nB=2\r\n")   # (as built, byte for byte)
        c.close()

    def test_moved_from_the_internal_storage_to_the_disk_and_back(self):
        self.ext1()
        write(self.there("data", "grapnel", "user", "save0.bin"), b"my save")
        c = self.console()
        p = gs.plan(c, self.out, self.nup)
        gs.send(c, self.out, self.nup, log=lambda *a: None, nup_at=p["nup_at"])
        # (to the disk: the title and the game file there, the old ones offered, removed only when asked)
        p = gs.plan(c, self.out, self.nup, "/mnt/ext1")
        self.assertEqual(p["title_elsewhere"], ["/data/homebrew/GRPN00001"])
        self.assertEqual(p["nup_elsewhere"], [("/data/grapnel/game.nup", 4 << 20)])
        self.assertTrue(p["nup"]["send"])   # (not on the disk yet: sent from the PC, ftpsrv has no copy between disks)
        self.assertEqual(gs.where(c)["title"], ["internal"])
        gs.send(c, self.out, self.nup, log=lambda *a: None, place="/mnt/ext1", nup_at=p["nup_at"])
        self.assertEqual(gs.verify(c, self.out, self.nup, "/mnt/ext1", p["nup_at"])[0], [])
        self.assertTrue(os.path.isdir(self.there("data", "homebrew", "GRPN00001")))   # (still there until asked)
        gs.remove_title(c, p["title_elsewhere"][0], self.out, "console")
        gs.remove_nup(c, p["nup_elsewhere"][0][0])
        self.assertEqual(os.listdir(self.there("data", "homebrew")), [])
        self.assertEqual(sorted(os.listdir(self.there("data", "grapnel"))), ["hookshot_env.txt", "image.bin", "user"])
        self.assertEqual(read(self.there("data", "grapnel", "user", "save0.bin")), b"my save")   # (the saves stay)
        self.assertEqual(gs.where(c), {"title": ["/mnt/ext1"], "nup": {"/mnt/ext1": 4 << 20}})
        self.assertIn("HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup", self.env_there())
        # (and back to the internal storage: the line goes)
        p = gs.plan(c, self.out, self.nup)
        self.assertEqual((p["title_elsewhere"], p["nup_elsewhere"]), (["/mnt/ext1/homebrew/GRPN00001"],
                                                                      [("/mnt/ext1/grapnel/game.nup", 4 << 20)]))
        gs.send(c, self.out, self.nup, log=lambda *a: None, nup_at=p["nup_at"])
        self.assertEqual(gs.verify(c, self.out, self.nup, gs.INTERNAL, p["nup_at"])[0], [])
        self.assertEqual(self.env_there(), "A=1\n")
        gs.remove_title(c, p["title_elsewhere"][0], self.out, "console")
        gs.remove_nup(c, p["nup_elsewhere"][0][0])
        self.assertEqual(os.listdir(self.there("mnt", "ext1", "homebrew")), ["OTHER0001"])   # (the other title stays)
        self.assertEqual(os.listdir(self.there("mnt", "ext1", "grapnel")), [])
        self.assertEqual(gs.where(c), {"title": ["internal"], "nup": {"internal": 4 << 20}})
        with open(os.path.join(self.out, "console-sent.json"), encoding="utf-8") as f:
            journal = json.load(f)["console"]
        self.assertFalse([k for k in journal if k.startswith("/mnt/ext1/homebrew/GRPN00001")])   # (forgotten with the folder)
        c.close()

    def test_the_game_file_not_sent_is_read_where_it_is(self):
        """The box for the game file left out: the game reads the copy there is, the place's first, else the internal one, and
        the hookshot_env.txt sent says so"""
        self.ext1()
        write(self.there("data", "grapnel", "game.nup"), read(self.nup))
        c = self.console()
        p = gs.plan(c, self.out, None, "/mnt/ext1")
        self.assertEqual((p["nup_at"], p["nup_elsewhere"]), ("/data/grapnel/game.nup", []))
        gs.send(c, self.out, None, log=lambda *a: None, place="/mnt/ext1", nup_at=p["nup_at"])
        self.assertNotIn("HOOKSHOT_PS5_NUP", self.env_there())
        write(self.there("mnt", "ext1", "grapnel", "game.nup"), read(self.nup))
        p = gs.plan(c, self.out, None, "/mnt/ext1")
        self.assertEqual(p["nup_at"], "/mnt/ext1/grapnel/game.nup")
        gs.send(c, self.out, None, log=lambda *a: None, place="/mnt/ext1", nup_at=p["nup_at"])
        self.assertIn("HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup", self.env_there())
        self.assertEqual(gs.verify(c, self.out, None, "/mnt/ext1", p["nup_at"])[0], [])
        c.close()

    def test_a_disk_with_no_homebrew_folder_yet(self):
        self.ext1(other=False)
        c = self.console()
        p = gs.plan(c, self.out, self.nup, "/mnt/ext1")
        gs.send(c, self.out, self.nup, log=lambda *a: None, place="/mnt/ext1", nup_at=p["nup_at"])
        self.assertEqual(gs.verify(c, self.out, self.nup, "/mnt/ext1", p["nup_at"])[0], [])
        self.assertEqual(sorted(os.listdir(self.there("mnt", "ext1"))), ["grapnel", "homebrew", "user"])
        c.close()

    def test_no_disk_no_send(self):
        os.makedirs(self.there("mnt", "ext1"))   # (a mount point with no disk)
        c = self.console()
        with self.assertRaises(gs.SendError):
            gs.plan(c, self.out, self.nup, "/mnt/ext1")
        with self.assertRaises(gs.SendError):
            gs.plan(c, self.out, self.nup, "/mnt/ext2")
        self.assertEqual(os.listdir(self.there("mnt", "ext1")), [])
        c.close()

    def test_nothing_else_on_the_disk(self):
        self.ext1()
        c = self.console()
        for p in ("/mnt/ext1/homebrew/OTHER0001/eboot.bin", "/mnt/ext1/other.bin", "/mnt/ext1/grapnelx/a", "/mnt/ext1/homebrew/x",
                  "/mnt/ext2/grapnel/game.nup", "/mnt/ext1/grapnel/../homebrew/OTHER0001/eboot.bin",
                  "/mnt/ext1/homebrew/GRPN00001/../OTHER0001/eboot.bin", "/mnt/ext1//homebrew/OTHER0001/eboot.bin"):
            with self.assertRaises(gs.SendError, msg=p):
                c.upload(self.nup, p)
            with self.assertRaises(gs.SendError, msg=p):
                c.delete(p)
        for p in ("/mnt/ext1", "/mnt/ext1/homebrew", "/mnt/ext1/homebrew/OTHER0001", "/mnt/ext3/homebrew/GRPN00001"):
            with self.assertRaises(gs.SendError, msg=p):
                c.rmdir(p)
        for p in ("/mnt/ext1/homebrew/OTHER0001", "/mnt/ext1/homebrew", "/data/homebrew", "/mnt/ext1/grapnel"):
            with self.assertRaises(gs.SendError, msg=p):
                gs.remove_title(c, p)
        for p in ("/mnt/ext1/grapnel/image.bin", "/data/grapnel/user/save0.bin", "/mnt/ext1/homebrew/OTHER0001/eboot.bin"):
            with self.assertRaises(gs.SendError, msg=p):
                gs.remove_nup(c, p)
        with self.assertRaises(gs.SendError):
            c.mkdirs("/mnt/ext2/grapnel")   # (never a mount point: a disk that is not there)
        self.assertEqual(read(self.there("mnt", "ext1", "homebrew", "OTHER0001", "eboot.bin")), b"another title")
        self.assertFalse(os.path.exists(self.there("mnt", "ext2")))
        c.close()

    def test_the_consoles_own_preparation_goes_with_the_data_folder(self):
        """(a PC whose processor has no AVX2: the build's step 7 set up the console's own preparation in data/grapnel,
        grapnel_core.setup_console_prepare) the census, the pipelines' list and the HOOKSHOT_PS5_PREPARE=1 line reach
        /data/grapnel, on the internal storage and with the game file on the extension disk (whose line is added beside it)"""
        import grapnel_core
        work = os.path.join(self.tmp.name, "work-shaders")
        census, states = os.path.join(work, "census.bin"), os.path.join(work, "states.bin")
        write(census, b"HKSEED01" + os.urandom(1 << 20))
        write(states, os.urandom(300 << 10))
        data = os.path.join(self.out, "data", "grapnel")
        r = grapnel_core.setup_console_prepare(data, census, states)
        self.assertEqual(r["files"], ["hookshot_shaders.bin", "hookshot_pipeline_states_shipped.bin"])
        c = self.console()
        gs.send(c, self.out, self.nup, log=lambda *a: None)
        self.assertEqual(gs.verify(c, self.out, self.nup)[0], [])
        self.assertEqual(read(self.there("data", "grapnel", "hookshot_shaders.bin")), read(census))
        self.assertEqual(read(self.there("data", "grapnel", "hookshot_pipeline_states_shipped.bin")), read(states))
        self.assertEqual(self.env_there(), read(os.path.join(data, "hookshot_env.txt")).decode())   # (as built)
        self.assertTrue(self.env_there().endswith("\nHOOKSHOT_PS5_PREPARE=1\n"))
        self.assertTrue(grapnel_core.console_prepare_ready(self.there("data", "grapnel")))
        c.close()
        self.ext1()
        c = self.console()
        p = gs.plan(c, self.out, self.nup, "/mnt/ext1")
        gs.send(c, self.out, self.nup, log=lambda *a: None, place="/mnt/ext1", nup_at=p["nup_at"])
        self.assertEqual(gs.verify(c, self.out, self.nup, "/mnt/ext1", p["nup_at"])[0], [])
        env = self.env_there()
        self.assertEqual(env.count("HOOKSHOT_PS5_PREPARE=1"), 1)
        self.assertIn("\nHOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup\n", env)
        self.assertEqual(grapnel_core.env_setting(self.there("data", "grapnel", "hookshot_env.txt"), "HOOKSHOT_PS5_PREPARE"), "1")
        self.assertTrue(os.path.isfile(self.there("data", "grapnel", "hookshot_shaders.bin")))   # (the data folder stays /data)
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
