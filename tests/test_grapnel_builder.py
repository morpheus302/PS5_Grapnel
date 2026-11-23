"""(grapnel) The Grapnel builder's core with no game: what it refuses and how it says it, the API a window drives, the compile's
progress, the title's identity, and the ZBIC program's own check.
    python -I -B tests/test_grapnel_builder.py
"""
import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts", "grapnel-builder"))
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
            self.assertEqual(core.prepare_tool(t)[0], kit)
            with open(os.path.join(kit, "longshot"), "wb") as f:
                f.write(b"HOST")
            with self.assertRaises(core.BuildError) as e:   # (a file changed)
                core.prepare_tool(t)
            self.assertEqual(e.exception.code, "prepare-tool")


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


if __name__ == "__main__":
    unittest.main()
