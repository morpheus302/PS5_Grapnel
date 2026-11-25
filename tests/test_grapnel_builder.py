"""(grapnel) The Grapnel builder's core with no game: what it refuses and how it says it, the API a window drives, the compile's
progress, the title's identity, and the ZBIC program's own check.
    python -I -B tests/test_grapnel_builder.py
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts", "grapnel-builder")
sys.path.insert(0, HERE)
# (the small packages here are not the game: no reference of its files, so none is said missing; tests/test_grapnel_game_files.py
# checks the reference's own work)
os.environ["GRAPNEL_GAME_FILES_REFERENCE"] = os.path.join(ROOT, "tests", "no-such-reference.ref.xz")
import grapnel_core as core  # noqa: E402

RUN_ALL_MODULES = ["zstandard"]


def fake_nup(path, title_id, version):
    """A NUP package with one title and an empty tree (nx2/test_nup_codec.py's layout): enough for the TOC check, no game content"""
    blob = bytearray(0x200)
    blob[:4] = b"NUP\x01"
    struct.pack_into("<Q", blob, 0x40, len(blob))
    blob[0x80:0x84] = b"TOC\x01"
    file_at = 0x80 + 0x30 + 0x40 + 24
    strings_at = file_at + 80 + 4
    struct.pack_into("<IQQQQ", blob, 0x84, 1, 1, 0, strings_at - 0x80, 0)
    struct.pack_into("<QIIII", blob, 0xb0, title_id, 0, 0, 0, version)
    struct.pack_into("<4I", blob, file_at - 24, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(blob)


def nup_with_files(path, title_id, version, files):
    """A NUP package with one title whose romfs is one folder of files [(name, stored bytes, logical size, codec)], the layout
    host/nup_fs.c reads (nx2/test_nup_codec.py's, with several files)"""
    names = b"".join(n.encode() for n, _, _, _ in files)
    nf = len(files)
    dirs_at = 0xb0 + 0x40
    files_at = dirs_at + 24
    kids_at = files_at + 80 * nf
    strings_at = kids_at + 4 * nf
    data_at = (strings_at + len(names) + 15) & ~15
    blob = bytearray(data_at + sum(len(d) for _, d, _, _ in files))
    blob[:4] = b"NUP\x01"
    struct.pack_into("<Q", blob, 0x40, len(blob))
    blob[0x80:0x84] = b"TOC\x01"
    struct.pack_into("<IQQQQ", blob, 0x84, 1, 1, nf, strings_at - 0x80, len(names))
    struct.pack_into("<QIIII", blob, 0xb0, title_id, 0, 0xffffffff, 0, version)
    struct.pack_into("<4I", blob, dirs_at, 0, 0, 0, nf)
    name_at, at = 0, data_at
    for i, (name, data, size, codec) in enumerate(files):
        struct.pack_into("<IIQQQ", blob, files_at + 80 * i, name_at, len(name), size, at, len(data))
        blob[files_at + 80 * i + 64] = codec
        struct.pack_into("<I", blob, kids_at + 4 * i, 1 + i)
        blob[at:at + len(data)] = data
        name_at += len(name)
        at += len(data)
    blob[strings_at:strings_at + len(names)] = names
    with open(path, "wb") as f:
        f.write(blob)


class Refusals(unittest.TestCase):
    def code(self, path):
        with self.assertRaises(core.BuildError) as c:
            core.check_nup(path)
        self.assertTrue(c.exception.message)
        return c.exception.code

    def test_folder_file_and_other_games(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.code(tmp), "nup-folder")
            self.assertEqual(self.code(os.path.join(tmp, "absent.nup")), "nup-missing")
            other = os.path.join(tmp, "image.bin")
            with open(other, "wb") as f:
                f.write(b"\x03\x00\x00\x14" + bytes(60))
            self.assertEqual(self.code(other), "nup-not-nup")
            game = os.path.join(tmp, "game.nup")
            fake_nup(game, 0x040074A01BF12000, 458752)   # (another game)
            self.assertEqual(self.code(game), "nup-wrong-game")
            fake_nup(game, core.GAME_TITLE_ID, 65536)    # (the game without its update)
            self.assertEqual(self.code(game), "nup-wrong-game")
            fake_nup(game, core.GAME_TITLE_ID, core.GAME_VERSION)
            self.assertEqual(core.check_nup(game), (core.GAME_TITLE_ID, core.GAME_VERSION))

    def test_compressed_game_files_get_a_flat_copy(self):
        # (0.2.3 refused a .nup whose game files are compressed: the console reads the game file in place (host/nup_fs.c) and opens
        # no compressed file, and a player's game stopped as Play was pressed (its log: 40,588 files with codec 1, Pack/ZsDic.pack.zs
        # not opened, 25/11/2026). The builder now takes it and gives the console a flat copy (grapnel_flat.py,
        # tests/test_grapnel_flat.py); a normal .nup is given as it is, with no copy made)
        import zstandard
        body = bytes(range(64)) * 2
        frame = zstandard.ZstdCompressor(write_content_size=True).compress(body)
        game_zs = zstandard.ZstdCompressor(write_content_size=True).compress(b"x" * 4000)   # (a .zs of the game: zstd in the game too)
        plain = [("f%03d.bin" % i, body, 128, 0) for i in range(80)] + [("t.zs", game_zs, len(game_zs), 0)]
        with tempfile.TemporaryDirectory() as tmp:
            game = os.path.join(tmp, "game.nup")
            facts = {}
            nup_with_files(game, core.GAME_TITLE_ID, core.GAME_VERSION, plain)
            self.assertEqual(core.check_nup(game, facts), (core.GAME_TITLE_ID, core.GAME_VERSION))
            self.assertFalse(facts["flat"])
            b = core.Builder(core.Options(nup=game, out=os.path.join(tmp, "out")))
            self.assertEqual(b.flatten(), game)   # (a normal .nup: itself, nothing written)
            self.assertFalse(os.path.exists(os.path.join(tmp, "out")))
            self.assertEqual(core.console_game_file(game, os.path.join(tmp, "out")), (game, False, ""))
            outer = zstandard.ZstdCompressor(write_checksum=True).compress(game_zs)   # (codec 1 over the game's .zs: ZsDic.pack.zs)
            coded = plain[:40] + [("c%03d.bin" % i, frame, 128, 1) for i in range(3)] + plain[40:80] + [("t.zs", outer, len(game_zs), 1)]
            nup_with_files(game, core.GAME_TITLE_ID, core.GAME_VERSION, coded)
            facts = {}
            self.assertEqual(core.check_nup(game, facts), (core.GAME_TITLE_ID, core.GAME_VERSION))
            self.assertTrue(facts["flat"])
            self.assertEqual(facts["coded"], 4)
            out = os.path.join(tmp, "out")
            self.assertEqual(core.console_game_file(game, out)[:2], (None, True))
            infos = []
            ev = core.Events()
            ev.info = infos.append
            b = core.Builder(core.Options(nup=game, out=out), ev)
            copy = b.flatten()
            self.assertEqual(copy, os.path.join(out, "game.nup"))
            self.assertTrue(any("keeps game files compressed" in i for i in infos))
            self.assertEqual(infos[0], core.FLAT_BANNER)   # (the player's wish: "Compression detected", first)
            self.assertIn("Compression detected", core.FLAT_BANNER)
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "grapnel_build.py"), "--flatten", "--nup", game, "--flat-dir",
                                os.path.join(tmp, "cli")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(r.stdout.splitlines()[0], core.FLAT_BANNER, r.stdout[:500])   # (the command line's first line)
            self.assertEqual(core.console_game_file(game, out), (copy, True, ""))
            facts = {}
            core.check_nup(copy, facts)   # (the copy is a game file the builder takes, and needs no copy)
            self.assertFalse(facts["flat"])
            import nup
            p = nup.Nup(copy, strict=True)
            try:
                got = {}
                for rel, r in p.walk(p.titles[0]["romfs"]):
                    self.assertEqual((r[5], r[4]), (0, r[2]))
                    got[rel] = p.read(r)
            finally:
                p.f.close()
            self.assertEqual(got["t.zs"], game_zs)   # (the outer layer gone, the game's .zs as it is)
            self.assertEqual(got["c001.bin"], body)
            self.assertTrue(core.Builder(core.Options(nup=game, out=out)).flatten() == copy)   # (taken again)
            framed = [(n, frame, 128, 0) for n, _, _, _ in plain[:80]] + plain[80:]
            nup_with_files(game, core.GAME_TITLE_ID, core.GAME_VERSION, framed)
            facts = {}
            core.check_nup(game, facts)
            self.assertTrue(facts["flat"] and facts["framed"] > 0 and facts["coded"] == 0)
            nup_with_files(game, 0x040074A01BF12000, 458752, framed)   # (another game: said as such first)
            self.assertEqual(self.code(game), "nup-wrong-game")
            odd = plain[:80] + [("u.bin", body, 128, 7)]   # (a codec no one knows)
            nup_with_files(game, core.GAME_TITLE_ID, core.GAME_VERSION, odd)
            self.assertEqual(self.code(game), "nup-codec")

    def test_window_says_compression_detected_first(self):
        # (the player's wish, 25/11/2026: the window's page for a compressed .nup says "Compression detected" ("Compression
        # détectée") in bold at its top, before the explanation, the room's check and the button)
        src = open(os.path.join(HERE, "grapnel_gui.py"), encoding="utf-8").read()
        self.assertIn('"flat_banner": "Compression détectée"', src)
        self.assertIn('"flat_banner": "Compression detected"', src)
        page = src[src.index("    def page_flat(self):"):src.index("    def pick_flat_dir(self):")]
        banner, heading = page.index('t["flat_banner"]'), page.index('self.heading(t["p_flat"]')
        self.assertLess(banner, heading)
        self.assertLess(banner, page.index("self.flat_btn"))
        self.assertIn("bold", page[:banner])

    def test_update_under_the_base_games_id(self):
        # (a player's dump on 24/11/2026: its newest title is v131072 under the base game's id, the update merged into it; their
        # Longshot builder took it, Grapnel 0.2.1 refused it. The console reads the newest title whatever its id, and step 1
        # checks the program's build after: the id is the base game's or its update's)
        base = 0x0400486024DEC000
        self.assertIn(base, core.GAME_TITLE_IDS)
        self.assertIn(core.GAME_TITLE_ID, core.GAME_TITLE_IDS)
        with tempfile.TemporaryDirectory() as tmp:
            game = os.path.join(tmp, "game.nup")
            fake_nup(game, base, core.GAME_VERSION)
            self.assertEqual(core.check_nup(game), (base, core.GAME_VERSION))
            fake_nup(game, base, 65536)                  # (the base game's id, without the update)
            self.assertEqual(self.code(game), "nup-wrong-game")
            fake_nup(game, 0x0400486024DEC001, core.GAME_VERSION)   # (another id at that version)
            self.assertEqual(self.code(game), "nup-wrong-game")

    def test_llvm_missing_says_how_to_get_it(self):
        with self.assertRaises(core.BuildError) as c:
            core.find_llvm("/nonexistent/llvm")
        self.assertEqual(c.exception.code, "llvm")
        self.assertIn(core.LLVM_VERSION, c.exception.message)
        self.assertIn("apt", c.exception.message)

    def test_steps_in_order(self):
        b = core.Builder(core.Options(nup="x.nup", out=tempfile.gettempdir()))
        for step in (b.lay_out, b.icons, b.shaders, b.prepare, b.write_notes):
            with self.assertRaises(core.BuildError) as c:
                step()
            self.assertEqual(c.exception.code, "order")


class Api(unittest.TestCase):
    def test_what_a_window_drives(self):
        for name in ("check", "read", "translate", "compile", "lay_out", "icons", "shaders", "prepare", "write_notes", "run"):
            self.assertTrue(callable(getattr(core.Builder, name)), name)
        for name in ("step", "progress", "log", "info", "step_done"):
            self.assertTrue(callable(getattr(core.Events, name)), name)
        self.assertEqual(len(core.STEPS), 8)
        self.assertEqual(core.STEPS.index("Listing the game's shaders"), 5)                # (step 6)
        self.assertEqual(core.STEPS.index("Preparing the shaders on this PC"), 6)          # (step 7, before the notes)
        e = core.BuildError("disk", "message", "details", "log")
        self.assertEqual((e.code, e.message, e.details, e.log), ("disk", "message", "details", "log"))

    def test_identity_and_memory(self):
        self.assertEqual(core.data_folder_name(), "grapnel")
        self.assertEqual(core.name_prefix(), "hookshot")
        self.assertEqual(core.toolchain_ps5.TITLE_ID, "GRPN00001")
        self.assertEqual(core.FLEXIBLE_MB, 580)
        self.assertTrue(os.path.isfile(os.path.join(core.DATA, "hookshot_env.txt")))
        with open(os.path.join(core.DATA, "skip.txt")) as f:
            self.assertEqual(f.read(), "528c39df51bae6e5\n")
        with open(os.path.join(core.DATA, "hookshot_env.txt")) as f:
            text = f.read()
        self.assertNotIn("LOG_UDP", text)   # (the public settings name no PC)
        self.assertIn("RX_VK_SKIP_FILE=/data/grapnel/skip.txt", text)
        # (lot 83: the shaders are prepared on the PC, the console only checks its cache: no preparation of its own offered, and the
        # real driver cache, the title folder's, which the builder fills)
        active = [l for l in text.splitlines() if l and not l.startswith("#")]
        self.assertFalse([l for l in active if l.startswith(("MESA_SHADER_CACHE_DIR", "HOOKSHOT_PS5_PREPARE"))])
        self.assertIn("HOOKSHOT_PS5_BACK_TO_LAUNCHER=1", active)
        self.assertIn("MESA_SHADER_CACHE_MAX_SIZE=4G", active)   # (the database's parts as the PC makes them: 50 in 4 GiB)

    def test_compile_progress(self):
        with tempfile.TemporaryDirectory() as gen:
            for name, mb in (("code_000.c", 2), ("code_hot_0.c", 1)):
                with open(os.path.join(gen, name), "wb") as f:
                    f.write(bytes(mb << 20))
            p = core.CompileProgress(gen)
            self.assertIsNone(p.line("version: x"))
            self.assertEqual(p.line("12 files, 8 jobs, -DA64_HOST_BASE=0x8000000000ULL"), 0.0)
            hot = p.line("  code_hot_0.c        1 MB      90s  (90s)")
            self.assertGreater(hot, 0.5)   # (a hot file weighs far more than its size says)
            last = p.line("  code_000.c          2 MB      20s  (110s)")
            self.assertGreater(last, hot)
            self.assertLessEqual(last, 0.99)


def fake_census(path, stages, programs):
    """A census as nx2/shader_census.py writes it: "HKSEED01", programs, stages; each stage's head (key, stage, control and code
    sizes), control, code; each program's stage indices"""
    with open(path, "wb") as f:
        f.write(b"HKSEED01" + struct.pack("<II", len(programs), len(stages)))
        for i, st in enumerate(stages):
            control, code = bytes(0xd8), bytes(16)
            f.write(struct.pack("<QIII", i + 1, st, len(control), len(code)) + control + code)
        for ids in programs:
            f.write(struct.pack("<I%dI" % len(ids), len(ids), *ids))


class Shaders(unittest.TestCase):
    """(lot 82) The two lists the console's own preparation reads: the census's shape checked as the renderer reads it, the
    pipelines' list made from the checkout alone, the census's files taken by shader_census.py's own rule"""

    def test_census_facts(self):
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "hookshot_shaders.bin")
            # stages: 0 vertex, 1 fragment, 5 compute, 7 mesh, 6 task
            fake_census(c, [0, 1, 5, 7, 6, 1], [(0, 1), (2,), (3, 5), (4, 3, 5)])
            f = core.census_facts(c)
            self.assertEqual((f["programs"], f["stages"], f["compute"], f["mesh"], f["mesh_task"]), (4, 6, 1, 2, 1))
            with open(c, "rb") as g:
                whole = g.read()
            for cut in (whole[:10], whole[:-3], b"HKSEED02" + whole[8:]):
                with open(c, "wb") as g:
                    g.write(cut)
                with self.assertRaises(core.BuildError) as e:
                    core.census_facts(c)
                self.assertEqual(e.exception.code, "census-wrong")
            fake_census(c, [0, 1], [(0, 7)])   # (a program naming a stage the census has not)
            with self.assertRaises(core.BuildError) as e:
                core.census_facts(c)
            self.assertEqual(e.exception.code, "census-wrong")

    def test_state_list_from_the_checkout(self):
        sys.path.insert(0, os.path.join(ROOT, "nx2"))
        import pipeline_states
        with tempfile.TemporaryDirectory() as tmp:
            dst = os.path.join(tmp, "hookshot_pipeline_states_shipped.bin")
            records, left = core.make_state_list(dst)
            self.assertGreater(records, 1000)
            self.assertGreater(left, 0)   # (the converted lists are there, and left out)
            recs = pipeline_states.read(dst)
            self.assertEqual(len(recs), records)
            with open(dst, "rb") as f:
                self.assertEqual(f.read(8), b"HKSTATE2")

    def test_census_files_by_its_own_rule(self):
        sys.path.insert(0, os.path.join(ROOT, "nx2"))
        import shader_census
        names = ["Shader/a.bfsha.zs", "Shader/ApplicationPackage/b.pck", "Lib/agl/c.sarc.zs", "Movie/Ndve/d.zs", "Movie/e.mp4",
                 "Effect/f.vsetb.byml.zs", "Effect/g.txt", "Model/h.bfres.zs", "Layout2Shader/i.zs", "Pack/ZsDic.pack.zs"]
        with tempfile.TemporaryDirectory() as romfs:
            for n in names:
                os.makedirs(os.path.dirname(os.path.join(romfs, n)), exist_ok=True)
                open(os.path.join(romfs, n), "wb").close()
            walked = {os.path.relpath(p, romfs).replace(os.sep, "/") for p in shader_census.archives(romfs)}
        ns = {}
        src = core.EXTRACT_CENSUS
        rule = src[src.index("    if not ("):src.index("        continue")]
        taken = set()
        for rel in names:
            name = rel.rsplit("/", 1)[-1]
            ns = {"rel": rel, "name": name, "folders": core.CENSUS_FOLDERS, "dictionary": core.CENSUS_DICT}
            exec(compile("def f():\n" + rule + "        return False\n    return True\n", "rule", "exec"), ns)
            if ns["f"]():
                taken.add(rel)
        self.assertEqual(taken, walked | {core.CENSUS_DICT})


def fake_db(part, blobs, index=None, cut=False):
    """A part of Mesa's cache database (src/util/mesa_cache_db.c): mesa_cache.db (head "MESA_DB", entries: 32-byte key, CRC,
    size, blob) and mesa_cache.idx (a 20-byte head, 28 bytes an entry)"""
    os.makedirs(part, exist_ok=True)
    with open(os.path.join(part, "mesa_cache.db"), "wb") as f:
        f.write(b"MESA_DB\0" + struct.pack("<IQ", 1, 7))
        for i, b in enumerate(blobs):
            f.write(bytes([i]) * 32 + struct.pack("<II", 0, len(b)) + b)
        if cut:
            f.write(bytes(32) + struct.pack("<II", 0, 100) + b"short")
    with open(os.path.join(part, "mesa_cache.idx"), "wb") as f:
        f.write(bytes(20) + bytes(28 * (len(blobs) if index is None else index)))


class Prepare(unittest.TestCase):
    """(lot 83) The shaders prepared on the PC: the tool's checks, the run's settings, the cache's and the progress file's reading"""

    def test_cache_read_whole(self):
        with tempfile.TemporaryDirectory() as cache:
            root = os.path.join(cache, "mesa_shader_cache_db")
            fake_db(os.path.join(root, "part0"), [b"a" * 10, b"b" * 300])
            fake_db(os.path.join(root, "part1"), [b"c"])
            f = core.mesa_db_facts(cache)
            self.assertEqual((f["parts"], f["entries"], f["bad"]), (2, 3, []))
            fake_db(os.path.join(root, "part2"), [b"d", b"e"], index=1)   # (the index has one entry less)
            fake_db(os.path.join(root, "part3"), [b"f"], cut=True)        # (an entry cut short)
            f = core.mesa_db_facts(cache)
            self.assertEqual(len(f["bad"]), 2)
            self.assertEqual(core.mesa_db_facts(os.path.join(cache, "none"))["entries"], 0)

    def test_progress_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "hookshot_prepare.txt")
            with open(p, "w") as f:
                f.write("grapnel-prepare 1\nstamp 00000000000000ab\ntranslator 1234\ndriver 5678\ntotal 21479\ndone 21479\n"
                        "finished 1\nstarted_at -1\nstarts 1\nseconds 600.0\nunits 1.0\nunits_done 1.0\nunits_total 1.0\n"
                        "inflight\nstrikes\nskip 4 9\n")
            r = core.read_progress(p)
            self.assertEqual((r["finished"], r["total"], r["skip"], r["inflight"]), ("1", "21479", [4, 9], []))
            self.assertEqual(core.read_progress(os.path.join(tmp, "none")), {})

    def test_the_runs_settings_are_the_consoles(self):
        env = core.prepare_env("/kit", "/run", "/cache", 14)
        with open(os.path.join(core.DATA, "hookshot_env.txt")) as f:
            public = dict(l.strip().split("=", 1) for l in f if l.strip() and not l.startswith("#"))
        for k, v in public.items():
            if k.startswith("HOOKSHOT_PS5_") or k in core.PREP_PLATFORM_ONLY:
                continue
            self.assertEqual(env.get(k), v, k)   # (every setting that can change a translation, as the console has it)
        # (nothing the PC's program stamp walks (vk_shader.cpp translator_stamp, off the console): the console's stamp then)
        walked = ("RX_VK_MMA", "RX_VK_NATIVE", "RX_VK_NAN", "RX_VK_TASK_CAP", "RX_VK_NETWORK", "RX_VK_GP_", "RX_VK_FOLLOW",
                  "RX_VK_NO_FOLLOW", "RX_VK_NO_F16", "RX_VK_SHARED", "RX_VK_MESH", "RX_VK_NO_INVARIANT", "RX_VK_MARKERS", "EDEN_")
        self.assertFalse([k for k in env if k.startswith(walked)])
        self.assertEqual((env["MESA_SHADER_CACHE_DIR"], env["MESA_DISK_CACHE_DATABASE"]), ("/cache", "1"))
        self.assertEqual((env["RX_VK_CONSOLE_PREPARE_RUN"], env["HOOKSHOT_PS5_PREPARE_WORKERS"]), ("1", "14"))
        self.assertEqual(env["VK_DRIVER_FILES"], os.path.join("/kit", "icd.json"))
        self.assertNotIn("HOOKSHOT_PS5_PREPARE", env)
        self.assertFalse([k for k in env if k.startswith("HOOKSHOT_PS5_") and k != "HOOKSHOT_PS5_PREPARE_WORKERS"])

    def test_time_left_by_steps(self):
        """The time left from a whole run's lines (under Wine, 14 threads, 1,359 s in all): within a third of the truth from the
        first step on, where the renderer's own figure said 1 h 18 at 313 s"""
        e = core.PrepEstimate()
        e.start_line("vk: prepare: the console's own preparation (from the start): 38809 census programs (1312 mesh, 1180 of "
                     "them with a task stage; 4128 compute), 48147 pipeline records in 16039 programs (48147 shipped); 21479 items, "
                     "from item 0 (no earlier preparation); 14 workers")
        truth = 1359
        for seconds, step, item, theirs in ((162, 1, 951, "1 h 07"), (182, 1, 1210, "59 min"), (192, 2, 1330, "54 min"),
                                            (313, 2, 1347, "1 h 18"), (464, 2, 1565, "50 min"), (524, 2, 5273, "20 min"),
                                            (534, 3, 6033, "18 min"), (615, 3, 7161, "11 min"), (917, 3, 11663, "5 min"),
                                            (1220, 3, 14483, "4 min")):
            frac, left = e.at(seconds, step, item, core.read_left(theirs))
            self.assertLess(abs(left - (truth - seconds)), max(120, (truth - seconds) / 3), (seconds, left))
            self.assertTrue(0 < frac < 1)
        again = core.PrepEstimate()   # (a run again, from an item past the first: the renderer's own figures)
        again.start_line("(1312 mesh, 1 of them; 4128 compute), 5 pipeline records in 16039 programs (x); 21479 items, from item 900 (")
        self.assertIsNone(again.at(100, 1, 1000))
        self.assertEqual((core.say_left(30), core.say_left(600), core.say_left(4200)), ("< 1 min", "10 min", "1 h 10"))
        self.assertEqual((core.read_left("< 1 min"), core.read_left("7 min"), core.read_left("1 h 09"), core.read_left("?")),
                         (30, 420, 4140, None))

    def test_progress_line(self):
        m = core.PREP_PROGRESS.search("[hos host] vk: prepare: 33 s: step 2 of 3, the compute programs, item 2081 of 21479 (31% of "
                                      "the work, 7 min left); 4128 pipelines built; 9120 MiB free")
        self.assertEqual((m.group(2), m.group(6), m.group(7), m.group(9)), ("2", "31", "7 min", "9120"))

    def test_the_tool_is_checked(self):
        with tempfile.TemporaryDirectory() as kit:
            t = {"prepare": kit}
            with self.assertRaises(core.BuildError) as e:
                core.prepare_tool(t)
            self.assertEqual(e.exception.code, "prepare-tool")
            if not sys.platform.startswith("linux"):
                return
            with open(os.path.join(kit, "longshot"), "wb") as f:
                f.write(b"host")
            info = {"name": "grapnel-prepare", "version": "0.1", "made": "2026-11-23", "shader_stamp": "0" * 16,
                    "driver_build_id": core.CONSOLE_DRIVER_BUILD_ID,
                    "files": {"longshot": {"bytes": 4, "sha256": core.sha256(os.path.join(kit, "longshot"))}}}
            with open(os.path.join(kit, "prepare.json"), "w") as f:
                json.dump(info, f)
            with self.assertRaises(core.BuildError) as e:   # (another renderer's translator: its cache is of no use)
                core.prepare_tool(t)
            self.assertEqual(e.exception.code, "prepare-stale")
            sys.path.insert(0, os.path.join(ROOT, "nx2"))
            import translator_files
            info["shader_stamp"] = translator_files.stamp(os.path.join(ROOT, "gpu", "vk"))
            with open(os.path.join(kit, "prepare.json"), "w") as f:
                json.dump(info, f)
            with self.assertRaises(core.BuildError) as e:   # (0.2.3: a preparation older than 0.3, whose gate stops a PC short of memory)
                core.prepare_tool(t)
            self.assertEqual(e.exception.code, "prepare-stale")
            self.assertIn("0.4 or newer", e.exception.message)
            info["version"] = "0.3"   # (0.2.4: 0.3's driver stops a CPU without CLFLUSHOPT at once)
            with open(os.path.join(kit, "prepare.json"), "w") as f:
                json.dump(info, f)
            with self.assertRaises(core.BuildError) as e:
                core.prepare_tool(t)
            self.assertEqual(e.exception.code, "prepare-stale")
            info["version"] = "0.4"
            with open(os.path.join(kit, "prepare.json"), "w") as f:
                json.dump(info, f)
            self.assertEqual(core.prepare_tool(t)[0], kit)
            with open(os.path.join(kit, "longshot"), "wb") as f:
                f.write(b"HOST")
            with self.assertRaises(core.BuildError) as e:   # (a file changed)
                core.prepare_tool(t)
            self.assertEqual(e.exception.code, "prepare-tool")

    def test_cache_digest_ignores_parts(self):
        """The cache's digest is its keys and blobs: which part holds an entry, and an entry written twice (the last one counts,
        as the index has it), change nothing; another blob does"""
        def db(entries):
            return b"MESA_DB" + b"\0" * 13 + b"".join(k + struct.pack("<II", 0, len(v)) + v for k, v in entries)
        k1, k2 = b"\1" * 32, b"\2" * 32
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            for root, parts in ((a, {"part0": [(k1, b"x"), (k2, b"old"), (k2, b"yy")]}), (b, {"part0": [(k2, b"yy")], "part1": [(k1, b"x")]})):
                for name, entries in parts.items():
                    os.makedirs(os.path.join(root, "mesa_shader_cache_db", name))
                    with open(os.path.join(root, "mesa_shader_cache_db", name, "mesa_cache.db"), "wb") as f:
                        f.write(db(entries))
            self.assertEqual(core.mesa_db_digest(a), core.mesa_db_digest(b))
            self.assertEqual(core.mesa_db_digest(a)[0], 2)
            with open(os.path.join(b, "mesa_shader_cache_db", "part1", "mesa_cache.db"), "wb") as f:
                f.write(db([(k1, b"z")]))
            self.assertNotEqual(core.mesa_db_digest(a), core.mesa_db_digest(b))

    def test_a_silent_run_is_ended(self):
        """A run that says nothing for idle seconds is ended, its exit SILENT_EXIT, which the preparation runs again (a run
        stopped once with all its threads waiting and the builder waited without end)"""
        b = core.Builder.__new__(core.Builder)
        b.env = dict(os.environ)
        b.ev = type("E", (), {"log": lambda self, line: None})()
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, "logs", "run.log")
            t0 = time.time()
            with self.assertRaises(core.BuildError) as e:
                b._run([sys.executable, "-c", "import time; print('start', flush=True); time.sleep(60)"], log, "a run", cwd=tmp, idle=2)
            self.assertLess(time.time() - t0, 30)
            self.assertIn("exit %d" % core.SILENT_EXIT, e.exception.message)
            self.assertIn("silent for 2 s: ended", open(log).read())

    def test_a_crashed_run_goes_on(self):
        """A preparation run that crashes (a Windows exception code, a signal on Linux) is run again from where it stopped,
        as a run short of memory is; an ordinary failure is not"""
        if os.name == "nt":
            self.assertTrue(core.crashed(0xC0000005) and core.crashed(0xC00000FD))
            self.assertFalse(core.crashed(3) or core.crashed(13))
        else:
            self.assertTrue(core.crashed(-11) and core.crashed(-6))
            self.assertTrue(core.illegal_instruction(-4) and not core.illegal_instruction(-11) and not core.illegal_instruction(13))
            self.assertFalse(core.crashed(3) or core.crashed(0) or core.crashed(13))
        core.quiet_crashes()   # (nothing on Linux; on Windows the error mode)

    def test_an_illegal_instruction_is_said_at_once(self):
        """(0.2.4) A preparation run that ends on an instruction the processor does not have is not run again (it would end the
        same way, six times): said at once as prepare-cpu; a crash still goes on. (0.2.6) Its message no longer sends to
        --no-prepare (which leaves the shaders to be compiled in play), but says the console prepares them at its first start,
        which the build sets up (tests/test_grapnel_cpu.py: step 7 then does)"""
        b = core.Builder.__new__(core.Builder)
        b.ev = type("E", (), {"info": lambda self, line: None, "progress": lambda self, *a: None})()
        ill = 0xC000001D if os.name == "nt" else -4
        segv = 0xC0000005 if os.name == "nt" else -11
        said = {}
        for code, want, runs in ((ill, "prepare-cpu", 1), (segv, "prepare-failed", core.PREP_TRIES)):
            calls = []

            def run(*a, **k):
                calls.append(1)
                raise core.BuildError("tool-failed", "the preparation ended (exit %d)" % code)
            b._run = run
            sleep, core.time.sleep = core.time.sleep, lambda s: None
            try:
                with self.assertRaises(core.BuildError) as e:
                    b._prepare_runs("run", {}, "log", "the preparation", 0.0, 1.0)
            finally:
                core.time.sleep = sleep
            self.assertEqual((e.exception.code, len(calls)), (want, runs))
            said[want] = e.exception.message
        self.assertNotIn("--no-prepare", said["prepare-cpu"])
        self.assertIn("console prepares the shaders itself at its first start", said["prepare-cpu"])
        self.assertIn("about an hour", said["prepare-cpu"])
        self.assertIn("exit %d" % ill, said["prepare-cpu"])

    def test_the_window_says_its_version(self):
        """(0.2.4) The window's title and its dialogs' titles carry the builder's release, read from grapnel_core.py"""
        sys.path.insert(0, os.path.join(ROOT, "scripts", "grapnel-builder"))
        try:
            import grapnel_gui as gui
        except ImportError:   # (no tkinter on this PC)
            self.skipTest("no tkinter")
        self.assertEqual(gui.builder_version(), core.VERSION)
        src = open(os.path.join(ROOT, "scripts", "grapnel-builder", "grapnel_gui.py"), encoding="utf-8").read()
        self.assertIn('self.t["title"] = "%s %s" % (self.t["title"], version)', src)
        self.assertIn('self.t["err_title"] = "%s %s" % (self.t["err_title"], version)', src)

    def test_no_git_on_the_pc(self):
        """A player's PC without git (most Windows PCs): the check of the preparation's renderer revision says nothing, and the
        build goes on (it raised FileNotFoundError in check(), before any step)"""
        with tempfile.TemporaryDirectory() as empty:
            old = os.environ.get("PATH")
            os.environ["PATH"] = empty
            try:
                self.assertEqual(core.checkout_tree("gpu/vk"), "")
            finally:
                os.environ["PATH"] = old

    def test_a_tool_for_another_system_is_refused(self):
        """(0.3) a toolchain's prepare/ is made for one system (make_prepare_tool.py --system): Windows' on Linux, or Linux's on
        Windows, is said, with --no-prepare as the way on"""
        here = "windows-x64" if os.name == "nt" else "linux-x64"
        other = "linux-x64" if here == "windows-x64" else "windows-x64"
        with tempfile.TemporaryDirectory() as kit:
            with open(os.path.join(kit, "prepare.json"), "w") as f:
                json.dump({"system": other, "files": {}, "shader_stamp": "0" * 16, "driver_build_id": core.CONSOLE_DRIVER_BUILD_ID}, f)
            with self.assertRaises(core.BuildError) as e:
                core.prepare_tool({"prepare": kit})
            self.assertEqual(e.exception.code, "prepare-platform")
            self.assertIn("--no-prepare", e.exception.message)

    def test_the_run_on_windows_opens_its_driver(self):
        """(0.3) on Windows the preparation opens the toolchain's vulkan_radeon.dll itself (GRAPNEL_VK_ICD, gpu/vk/vk_main.cpp) with
        Windows' own folders on PATH; Linux's run keeps its loader and lib/"""
        env = core.prepare_env("/kit", "/run", "/cache", 4)
        if os.name == "nt":
            self.assertEqual(env["GRAPNEL_VK_ICD"], os.path.join(os.path.abspath("/kit"), "vulkan_radeon.dll"))
            self.assertNotIn("LD_LIBRARY_PATH", env)
            self.assertEqual(core.PREP_PROGRAMS, ("longshot.exe", "longshot_vk.dll"))
            self.assertEqual(env["MESA_SHADER_CACHE_DIR"], os.path.relpath(os.path.abspath("/cache"), os.path.abspath("/run")))
        else:
            self.assertEqual(env["LD_LIBRARY_PATH"], os.path.join("/kit", "lib"))
            self.assertNotIn("GRAPNEL_VK_ICD", env)
            self.assertEqual(core.PREP_PROGRAMS, ("longshot", "longshot_vk.so"))
        with open(os.path.join(ROOT, "gpu", "vk", "vk_main.cpp"), encoding="utf-8") as f:
            main = f.read()
        self.assertIn('getenv("GRAPNEL_VK_ICD")', main)
        self.assertIn("#if defined(_WIN32) && defined(HOOKSHOT_CACHE_BUILD)", main)   # (the game itself keeps System32's loader)


class Assistant(unittest.TestCase):
    def test_a_game_of_another_builder_is_not_offered(self):
        """(0.3-test2, the player's PC) an output folder whose game an earlier test package built: the assistant went straight
        to the shaders and the game was compiled again under the shaders' page. Only this builder's game counts as built"""
        try:
            import grapnel_gui as gui
        except ImportError:   # (no Tkinter here)
            self.skipTest("no tkinter")
        with tempfile.TemporaryDirectory() as out:
            os.makedirs(os.path.join(out, "homebrew", "GRPN00001"))
            with open(os.path.join(out, "homebrew", "GRPN00001", "eboot.bin"), "wb") as f:
                f.write(b"eboot")
            entry = {"bytes": 5, "crc32": 1}
            for checkout, want in (("v3.0.0-rc.5-328-ga0342b72 (source 0cafb0b3)", None), ("v9 (source 1)", entry)):
                with open(os.path.join(out, "manifest.json"), "w") as f:
                    json.dump({"checkout": checkout, "files": {"homebrew/GRPN00001/eboot.bin": entry}}, f)
                old = gui.this_version
                gui.this_version = lambda: "v9"
                try:
                    self.assertEqual(gui.built(out)["game"], want, checkout)
                finally:
                    gui.this_version = old


    def test_step6_finds_the_extension_disk_and_the_title(self):
        """Step 6's connection test asks the console's FTP server for its extension disks and where the title is (the page
        then offers the disk and chooses the title's place by default)"""
        try:
            import grapnel_gui as gui
        except ImportError:
            self.skipTest("no tkinter")
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import ftp_fake
        with tempfile.TemporaryDirectory() as t:
            jail = os.path.realpath(t)
            os.makedirs(os.path.join(jail, "mnt", "ext0"))
            os.makedirs(os.path.join(jail, "mnt", "ext1", "homebrew", "GRPN00001"))
            os.makedirs(os.path.join(jail, "data", "grapnel"))
            with open(os.path.join(jail, "data", "grapnel", "game.nup"), "wb") as f:
                f.write(b"nup")
            server = ftp_fake.Server(jail)
            port = server.start()
            old = os.environ.get("GRAPNEL_TEST_CONSOLE_ROOT")
            os.environ["GRAPNEL_TEST_CONSOLE_ROOT"] = jail
            try:
                found = gui.probe_places("127.0.0.1", port)
            finally:
                server.shutdown()
                server.server_close()
                if old is None:
                    os.environ.pop("GRAPNEL_TEST_CONSOLE_ROOT")
                else:
                    os.environ["GRAPNEL_TEST_CONSOLE_ROOT"] = old
            self.assertTrue(found["ftp"])
            self.assertEqual(found["volumes"], ["/mnt/ext1"])
            self.assertEqual(found["where"], {"title": ["/mnt/ext1"], "nup": {"internal": 3}})

    def test_a_work_folder_is_known(self):
        try:
            import grapnel_gui as gui
        except ImportError:
            self.skipTest("no tkinter")
        with tempfile.TemporaryDirectory() as out:
            self.assertFalse(gui.has_work(out))
            self.assertFalse(gui.has_work(""))
            os.makedirs(os.path.join(out, "grapnel-work", "sa"))
            self.assertTrue(gui.has_work(out))


class CompileLines(unittest.TestCase):
    def test_title_folder_with_spaces(self):
        # (a Windows player's folder: C:\Users\<first and last name>\..., spaces in it; seen on a real Windows PC, 23/11/2026)
        line = ("ps5: ==> GRPN00001 (game): C:\\Users\\A Player Name\\Documents\\Grapnel\\grapnel-work\\sa\\build-ps5\\ps5\\"
                "title-game\\GRPN00001, eboot.bin 500509293 bytes; RADV 3e6ee682381 (H:\\Grapnel Builder\\Grapnel\\toolchain\\radv"
                "\\lib\\libvulkan_radeon.ps5.a)")
        m = core.TITLE_LINE.search(line)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), "GRPN00001")
        self.assertEqual(m.group(2), "C:\\Users\\A Player Name\\Documents\\Grapnel\\grapnel-work\\sa\\build-ps5\\ps5\\"
                                     "title-game\\GRPN00001")
        self.assertEqual(int(m.group(3)), 500509293)
        m = core.TITLE_LINE.search("ps5: ==> GRPN00001 (game): /home/p/w/sa/build-ps5/ps5/title-game/GRPN00001, eboot.bin 12 bytes; "
                                   "RADV x (y)")
        self.assertEqual(m.group(2), "/home/p/w/sa/build-ps5/ps5/title-game/GRPN00001")


class ZbicProgram(unittest.TestCase):
    def test_selftest(self):
        import shutil
        if not shutil.which("clang"):
            self.skipTest("no clang to build nx2/zbic_unpack")
        sys.path.insert(0, os.path.join(ROOT, "nx2"))
        import zbic
        self.assertTrue(zbic.selftest().startswith("ok"))
        with self.assertRaises(ValueError):
            zbic.decompress(b"ZBIC not a frame", 100)
        self.assertIn("through the pipes", zbic.selftest())   # (every byte value both ways: Windows' text streams broke them)


class ObjectsOfAnotherLlvm(unittest.TestCase):
    """Two builds of LLVM 19.1.7 compiled vk_texture.cpp otherwise (llvm-mingw's on libc++, every other on libstdc++), and the
    object keys do not name the compiler: a work folder compiled by another LLVM, or by a builder that kept no mark, is
    compiled again; one compiled by this LLVM, even halfway, keeps its objects"""

    def test_mark(self):
        with tempfile.TemporaryDirectory() as work:
            b = os.path.join(work, "bin")
            os.makedirs(b)
            with open(os.path.join(b, "clang.exe"), "wb") as f:
                f.write(b"x" * 10)
            a = core.llvm_stamp({"bindir": b, "version": "clang version 19.1.7 (llvm-mingw)"})
            w = core.llvm_stamp({"bindir": b, "version": "(built by Brecht Sanders, r3) clang version 19.1.7"})
            self.assertNotEqual(a, w)
            mark = os.path.join(work, "sa", "grapnel-llvm.txt")
            self.assertFalse(core.objects_from_another_llvm(work, mark, w))   # (nothing compiled yet)
            os.makedirs(os.path.join(work, "sa", "build-ps5", "obj"))
            self.assertTrue(core.objects_from_another_llvm(work, mark, w))    # (a builder that kept no mark)
            with open(mark, "w", encoding="utf-8") as f:
                f.write(a)
            self.assertTrue(core.objects_from_another_llvm(work, mark, w))    # (another LLVM)
            with open(mark, "w", encoding="utf-8") as f:
                f.write(w)
            self.assertFalse(core.objects_from_another_llvm(work, mark, w))   # (this one: a stopped build resumes)


class DirectToolchain(unittest.TestCase):
    """(grapnel, Windows) the console toolchain without bash: nx2/toolchain_ps5.py's direct mode, from a toolchain's recipe"""

    RECIPE = {"format": 1, "llvm": "19.1.7", "radv": "radv/lib/libvulkan_radeon.ps5.a", "builtins": "llvm-rt/libclang_rt.builtins-x86_64.a",
              "compile": {"c": {"before": ["-target", "x86_64-sie-ps5", "-isysroot", "{sdk}", "-isystem", "{sdk}/target/include"],
                                "after": ["--sysroot", "{sdk}", "-lkernel_web"]},
                          "c++": {"before": ["-target", "x86_64-sie-ps5", "-stdlib++-isystem", "{sdk}/target/include/c++/v1"],
                                  "after": ["--sysroot", "{sdk}"]}},
              "crt": {"before": ["-target", "x86_64-sie-ps5", "-isysroot", "{sdk}"]},
              "lld": {"pie+script": {"before": ["-m", "elf_x86_64", "-pie", "-T{sdk}/ldscripts/elf_x86_64.x"], "after": ["--hash-style=gnu"]},
                      "pie": {"before": ["-m", "elf_x86_64", "-pie"], "after": ["--hash-style=gnu"]},
                      "nopie+script": {"before": ["-m", "elf_x86_64", "-T{sdk}/ldscripts/elf_x86_64.x"], "after": ["--hash-style=gnu"]}},
              "broker_link": ["--sysroot={sdk}", "-o", "{out}", "{obj}", "-lc"],
              "link": {"script": ["-T", "{vk}/tooling/psbc/ps5-pie-unwind.ld"], "flags": ["--version-script", "{map}"],
                       "inputs": ["{radv}", "{builtins}"], "map": "{ local: x; };\n"}}

    def setUp(self):
        import json
        self.tmp = tempfile.TemporaryDirectory()
        self.top = self.tmp.name
        with open(os.path.join(self.top, "ps5_recipe.json"), "w") as f:
            json.dump(self.RECIPE, f)
        self.env = {k: os.environ.get(k) for k in ("PS5_RECIPE", "PS5_LLVM_BIN")}
        os.environ.update(PS5_RECIPE=os.path.join(self.top, "ps5_recipe.json"), PS5_LLVM_BIN=os.path.join(self.top, "bin"))
        os.makedirs(os.path.join(self.top, "bin"))
        for n in ("clang", "ld.lld", "llvm-ar", "llvm-nm"):
            open(os.path.join(self.top, "bin", n + (".exe" if os.name == "nt" else "")), "w").close()
        self.t = core.toolchain_ps5

    def tearDown(self):
        for k, v in self.env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.tmp.cleanup()

    def test_compiles_as_the_wrappers(self):
        t, sdk = self.t, os.path.join(self.top, "sdk")
        self.assertTrue(t.direct())
        c = t.command(t.find("clang"), ["-O2", "-c", "a.c", "-o", "a.o"])
        self.assertEqual(c[0], os.path.join(self.top, "bin", "clang" + (".exe" if os.name == "nt" else "")))
        self.assertEqual(c[1:], t.forward_slashes(["-target", "x86_64-sie-ps5", "-isysroot", sdk, "-isystem",
                                                   os.path.join(sdk, "target", "include"), "-O2", "-c", "a.c", "-o", "a.o",
                                                   "--sysroot", sdk, "-lkernel_web"]))
        cxx = t.command(t.find("clang++"), ["-c", "a.cpp", "-o", "a.o"])
        self.assertEqual(cxx[0], c[0])   # (C++ is clang in its g++ mode: Windows' LLVM has no clang++.exe)
        self.assertEqual(cxx[1], "--driver-mode=g++")
        with self.assertRaises(RuntimeError):   # (a link through the driver would look for prospero-lld)
            t.command(t.find("clang"), ["-o", "a.elf", "a.c"])

    def test_lld_cases(self):
        t = self.t
        lld = t.find("lld")
        self.assertIn("-pie", t.command(lld, ["-o", "x", "y.o"]))
        self.assertTrue(any(a.startswith("-T") for a in t.command(lld, ["-o", "x", "y.o"])))
        main = t.command(lld, ["-T", "my.ld", "-o", "x", "y.o"])
        self.assertIn("-pie", main)
        self.assertEqual(sum(a.startswith("-T") for a in main), 1)   # (the command's script only)
        shared = t.command(lld, ["--shared", "-o", "x.so", "y.o"])
        self.assertNotIn("-pie", shared)
        self.assertEqual(shared[-1], "--hash-style=gnu")

    def test_link_recipe_from_data(self):
        t = self.t
        with tempfile.TemporaryDirectory() as work:
            script, flags, inputs = t._recipe_data(work)
            mapfile = os.path.join(work, "radv-platform-local.map")
            self.assertEqual(flags, ["--version-script", mapfile])
            with open(mapfile) as f:
                self.assertEqual(f.read(), "{ local: x; };\n")
        self.assertEqual(inputs, [os.path.join(self.top, "radv", "lib", "libvulkan_radeon.ps5.a"),
                                  os.path.join(self.top, "llvm-rt", "libclang_rt.builtins-x86_64.a")])
        self.assertEqual(script[1], os.path.join(self.top, "ps5_vulkan", "tooling", "psbc", "ps5-pie-unwind.ld"))

    def test_lld_one_thread_on_windows(self):
        """Under Wine the Windows builder's ld.lld hung with its threads, never with one: on Windows every link takes one,
        unless the command says otherwise"""
        t = self.t
        real = t.os.name
        try:
            t.os.name = "nt"
            lld = os.path.join(self.top, "bin", "ld.lld.exe")
            self.assertIn("--threads=1", t.command(lld, ["--shared", "-o", "x.so", "y.o"]))
            main = t.command(lld, ["--threads=%d" % t.link_threads(), "-T", "a.ld", "-o", "x", "y.o"])
            self.assertEqual([a for a in main if a.startswith("--threads")], ["--threads=1"])
        finally:
            t.os.name = real
        self.assertNotIn("--threads=1", t.command(t.find("lld"), ["--shared", "-o", "x.so", "y.o"]))   # (Linux: as it was)
        self.assertEqual(t.link_threads(), 3)

    def test_param_json_lf(self):
        """param.json, which the console reads, is the same file on Windows: open() here ends lines with CRLF unless told
        otherwise, as Windows' Python does"""
        import builtins
        t, real = self.t, builtins.open

        def windows_open(f, mode="r", *a, **kw):
            if "b" not in mode and "newline" not in kw and len(a) < 3:
                kw["newline"] = "\r\n"
            return real(f, mode, *a, **kw)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "param.json")
            t.open = windows_open
            try:
                t.write_param_json(path)
            finally:
                del t.open
            with real(path, "rb") as f:
                data = f.read()
        self.assertNotIn(b"\r", data)
        self.assertEqual(data, t.param_json().encode("utf-8"))

    def test_linker_script_is_utf8(self):
        """The link's script names the toolchain's folder (INCLUDE), and lld reads it as UTF-8: written in Windows' code page,
        a folder named "Prénom" was not found (0.2-test2 under Wine). open() here writes cp1252 unless told otherwise, as
        Windows' Python does"""
        import builtins
        t, real = self.t, builtins.open

        def cp1252_open(f, mode="r", *a, **kw):
            if "b" not in mode and "encoding" not in kw and len(a) < 2:
                kw["encoding"] = "cp1252"
            return real(f, mode, *a, **kw)
        with tempfile.TemporaryDirectory() as work:
            t.open = cp1252_open
            try:
                args = t.dirty_script(work, ["-T", os.path.join(work, "Prénom Nom", "ps5-pie-unwind.ld"), "-L", work])
            finally:
                del t.open
            with real(args[args.index("-T") + 1], "rb") as f:
                self.assertIn("Prénom Nom".encode("utf-8"), f.read())

    def test_windows_paths_written_with_slashes(self):
        t = self.t
        real = t.os.sep
        try:
            t.os.sep = "\\"
            self.assertEqual(t.forward_slashes(["-IC:\\g\\host", "C:\\g\\nx2\\..\\gpu\\a.cpp", '-DX="a\\b"']),
                             ["-IC:/g/host", "C:/g/nx2/../gpu/a.cpp", '-DX="a\\b"'])
        finally:
            t.os.sep = real


class DirectAgainstWrappers(unittest.TestCase):
    """With LLVM 19 and a toolchain 0.2 folder (GRAPNEL_TOOLCHAIN): clang's -### for the direct command is the wrapper's"""

    def test_same_cc1(self):
        import shutil
        import subprocess
        tc, llvm = os.path.abspath(os.environ.get("GRAPNEL_TOOLCHAIN") or "."), "/usr/lib/llvm-19/bin"
        if not os.path.isfile(os.path.join(tc, "ps5_recipe.json")) or not os.path.isfile(os.path.join(llvm, "clang")) or \
                not shutil.which("bash"):
            self.skipTest("GRAPNEL_TOOLCHAIN (a 0.2 toolchain folder), LLVM 19 and bash are needed")
        t = core.toolchain_ps5
        saved = {k: os.environ.get(k) for k in ("PS5_RECIPE", "PS5_LLVM_BIN")}
        try:
            os.environ.update(PS5_RECIPE=os.path.join(tc, "ps5_recipe.json"), PS5_LLVM_BIN=llvm)
            env = dict(os.environ, LLVM_CONFIG=os.path.join(llvm, "llvm-config"))
            with tempfile.TemporaryDirectory() as tmp:
                for name, src in (("clang", "a.c"), ("clang++", "a.cpp")):
                    with open(os.path.join(tmp, src), "w") as f:
                        f.write("int f(void) { return 1; }\n")
                    args = ["-O2", "-march=x86-64-v3", "-c", src, "-o", "a.o", "-###"]
                    cc1 = []
                    for cmd in ([os.path.join(tc, "sdk", "bin", "prospero-" + name)] + args, t.command(t.find(name), args)):
                        r = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=tmp)
                        line = next((l for l in r.stderr.splitlines() if '"-cc1"' in l), None)
                        self.assertIsNotNone(line, r.stderr[-500:])
                        cc1.append(line.split('" "', 1)[1])
                    self.assertEqual(cc1[0], cc1[1], name)
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v


if __name__ == "__main__":
    unittest.main()
