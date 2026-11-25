#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""(grapnel) A PC whose processor has no x86-64-v3 (AVX2...), which the toolchain's shader preparation for the PC is compiled
for (make_prepare_tool.py -march=x86-64-v3): a player's Intel Celeron N4120 made the whole build, then the preparation stopped
on an instruction the processor does not have (0xC000001D, 25/11/2026). Now the check reads the processor (/proc/cpuinfo on
Linux; IsProcessorFeaturePresent, then numpy's own reading, on Windows; GRAPNEL_CPU_V3=0/1 says it instead), says it as a
note at step 3, and the build's step 7 sets up the console's own preparation by itself (the census and the pipelines' list
in data/grapnel, HOOKSHOT_PS5_PREPARE=1 once in its hookshot_env.txt); INSTALL says the console prepares the shaders at its
first start. A run that still stops on such an instruction goes the same way. The window's check line (Processor), in
French and English. No game, no console, no GPU.

    python -I -B tests/test_grapnel_cpu.py
"""
import builtins
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts", "grapnel-builder")
sys.path.insert(0, HERE)
import grapnel_core as core  # noqa: E402

# (/proc/cpuinfo flags lines: a Haswell's, the player's Celeron N4120 (Goldmont Plus: SSE4.2, MOVBE, no AVX at all), an AMD
# FX (Piledriver: AVX, FMA, F16C, BMI1, no AVX2, BMI2 or MOVBE))
HASWELL = ("fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss ht syscall nx "
           "pdpe1gb rdtscp lm constant_tsc pni pclmulqdq ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt aes xsave avx f16c rdrand "
           "lahf_lm abm bmi1 avx2 smep bmi2 erms invpcid xsaveopt")
CELERON_N4120 = ("fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ss "
                 "ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc pni pclmulqdq ssse3 cx16 sse4_1 sse4_2 x2apic movbe "
                 "popcnt aes xsave rdrand lahf_lm 3dnowprefetch fsgsbase smep erms mpx rdseed smap clflushopt sha_ni xsaveopt "
                 "xsavec xgetbv1 xsaves")
PILEDRIVER = ("fpu sse sse2 ht syscall nx lm pni pclmulqdq ssse3 fma cx16 sse4_1 sse4_2 popcnt aes xsave avx f16c lahf_lm abm "
              "sse4a xop fma4 tbm bmi1")


def cpuinfo(*flags, name="Some CPU"):
    return "".join("processor\t: %d\nvendor_id\t: GenuineIntel\nmodel name\t: %s\nflags\t\t: %s\nvmx flags\t: ept\n\n"
                   % (i, name, f) for i, f in enumerate(flags))


class Events(core.Events):
    def __init__(self):
        self.infos, self.logs = [], []

    def info(self, text):
        self.infos.append(text)

    def log(self, line):
        self.logs.append(line)


class Forced:
    """GRAPNEL_CPU_V3 set (or unset with None) for a block, the environment as it was afterwards"""

    def __init__(self, value):
        self.value = value

    def __enter__(self):
        self.old = os.environ.get(core.CPU_V3_ENV)
        if self.value is None:
            os.environ.pop(core.CPU_V3_ENV, None)
        else:
            os.environ[core.CPU_V3_ENV] = self.value

    def __exit__(self, *exc):
        if self.old is None:
            os.environ.pop(core.CPU_V3_ENV, None)
        else:
            os.environ[core.CPU_V3_ENV] = self.old


def on_linux(text):
    """cpu_facts() as on Linux with this /proc/cpuinfo (None: it cannot be read)"""
    real_open = builtins.open

    def fake_open(path, *a, **k):
        if path == "/proc/cpuinfo":
            if text is None:
                raise PermissionError("not allowed")
            return io.StringIO(text)
        return real_open(path, *a, **k)
    with Forced(None), mock.patch.object(core.os, "name", "posix"), mock.patch.object(core.sys, "platform", "linux"), \
            mock.patch.object(builtins, "open", fake_open):
        return core.cpu_facts()


def on_windows(present, numpy=None, fails=False):
    """cpu_facts() as on Windows: IsProcessorFeaturePresent says yes for the features in present (or fails), numpy's reading
    is numpy (None: no numpy)"""
    ctypes = mock.MagicMock()
    if fails:
        ctypes.windll.kernel32.IsProcessorFeaturePresent.side_effect = OSError("kernel32 not there")
    else:
        ctypes.windll.kernel32.IsProcessorFeaturePresent.side_effect = lambda pf: int(pf in present)
    with Forced(None), mock.patch.object(core.os, "name", "nt"), mock.patch.dict(sys.modules, {"ctypes": ctypes}), \
            mock.patch.object(core, "numpy_cpu_features", lambda: numpy), \
            mock.patch.object(core, "windows_cpu_name", lambda: "Intel(R) Celeron(R) N4120 CPU @ 1.10GHz"):
        return core.cpu_facts()


class Detection(unittest.TestCase):
    def test_cpuinfo_with_and_without_the_flags(self):
        self.assertEqual(core.cpu_v3_from_cpuinfo(cpuinfo(HASWELL, HASWELL, name="Intel(R) Core(TM) i5-4670K CPU @ 3.40GHz")),
                         (True, [], "Intel(R) Core(TM) i5-4670K CPU @ 3.40GHz"))
        v3, missing, name = core.cpu_v3_from_cpuinfo(cpuinfo(CELERON_N4120, name="Intel(R) Celeron(R) N4120 CPU @ 1.10GHz"))
        self.assertEqual((v3, missing, name), (False, ["avx2", "bmi1", "bmi2", "fma", "f16c", "abm"],
                                               "Intel(R) Celeron(R) N4120 CPU @ 1.10GHz"))
        self.assertEqual(core.cpu_v3_from_cpuinfo(cpuinfo(PILEDRIVER))[:2], (False, ["avx2", "bmi2", "movbe"]))
        # (every processor counts: one without AVX2 is enough to say no; no flags line at all, another architecture's, is no)
        self.assertEqual(core.cpu_v3_from_cpuinfo(cpuinfo(HASWELL, HASWELL.replace(" avx2", "")))[:2], (False, ["avx2"]))
        self.assertFalse(core.cpu_v3_from_cpuinfo("processor\t: 0\nFeatures\t: fp asimd evtstrm aes\nCPU part\t: 0xd0c\n")[0])
        self.assertFalse(core.cpu_v3_from_cpuinfo("")[0])
        for flag in core.CPU_V3_FLAGS:   # (each of the seven is needed)
            self.assertFalse(core.cpu_v3_from_cpuinfo(cpuinfo(HASWELL.replace(" %s " % flag, " ")))[0], flag)

    def test_linux_reads_proc_cpuinfo(self):
        yes = on_linux(cpuinfo(HASWELL, name="Intel(R) Core(TM) i5-4670K CPU @ 3.40GHz"))
        self.assertEqual((yes["v3"], yes["forced"], yes["name"]), (True, False, "Intel(R) Core(TM) i5-4670K CPU @ 3.40GHz"))
        self.assertIn("/proc/cpuinfo", yes["read"])
        no = on_linux(cpuinfo(CELERON_N4120, name="Intel(R) Celeron(R) N4120 CPU @ 1.10GHz"))
        self.assertFalse(no["v3"])
        self.assertIn("missing: avx2 bmi1 bmi2 fma f16c abm", no["read"])
        unread = on_linux(None)   # (what cannot be read is a no: the console prepares, nothing stops)
        self.assertFalse(unread["v3"])
        self.assertIn("not read", unread["read"])

    def test_windows_never_a_wrong_yes(self):
        """Windows: IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE, 40); when it says no, numpy's own reading (AVX2,
        FMA3 and F16C, all three); anything not read is a no"""
        yes = on_windows({core.PF_AVX_INSTRUCTIONS_AVAILABLE, core.PF_AVX2_INSTRUCTIONS_AVAILABLE})
        self.assertTrue(yes["v3"])
        self.assertIn("AVX2 (40) yes", yes["read"])
        self.assertEqual(yes["name"], "Intel(R) Celeron(R) N4120 CPU @ 1.10GHz")
        celeron = {"SSE42": True, "AVX": False, "AVX2": False, "FMA3": False, "F16C": False}
        no = on_windows(set(), celeron)
        self.assertFalse(no["v3"])
        self.assertIn("AVX2 (40) no", no["read"])
        self.assertIn("numpy's reading of the processor: AVX2 no, FMA3 no, F16C no", no["read"])
        # (a Windows that does not fill the AVX2 bit: numpy's reading decides; AVX2 alone without FMA3 is not taken)
        self.assertTrue(on_windows(set(), {"AVX": True, "AVX2": True, "FMA3": True, "F16C": True})["v3"])
        self.assertFalse(on_windows({core.PF_AVX_INSTRUCTIONS_AVAILABLE}, {"AVX": True, "AVX2": True, "FMA3": False, "F16C": True})["v3"])
        self.assertFalse(on_windows(set(), None)["v3"])
        self.assertIn("not there", on_windows(set(), None)["read"])
        self.assertFalse(on_windows(set(), None, fails=True)["v3"])
        self.assertTrue(on_windows(set(), {"AVX2": True, "FMA3": True, "F16C": True}, fails=True)["v3"])

    def test_numpy_reads_this_processor(self):
        """numpy's reading (the second source on Windows) agrees with /proc/cpuinfo here"""
        features = core.numpy_cpu_features()
        if features is None or not sys.platform.startswith("linux"):
            self.skipTest("no numpy, or not Linux")
        with open("/proc/cpuinfo") as f:
            v3 = core.cpu_v3_from_cpuinfo(f.read())[0]
        self.assertEqual(all(features.get(k) for k in ("AVX2", "FMA3", "F16C")), v3)

    def test_the_setting_says_it_instead(self):
        for value, want in (("0", False), ("1", True)):
            with Forced(value):
                facts = core.cpu_facts()
            self.assertEqual((facts["v3"], facts["forced"]), (want, True))
            self.assertIn("%s=%s" % (core.CPU_V3_ENV, value), facts["read"])
            self.assertIn("%s=%s says so" % (core.CPU_V3_ENV, value), core.cpu_line(facts))
        with Forced("yes"):   # (not 0 or 1: the processor is read, and the log says the setting was not taken)
            facts = core.cpu_facts()
        self.assertFalse(facts["forced"])
        self.assertIn("is not 0 or 1: not taken", facts["read"])

    def test_the_checks_line(self):
        ok = core.cpu_line({"v3": True, "name": "AMD Ryzen 5 2600 Six-Core Processor"})
        self.assertTrue(ok.startswith("ok: the processor (AMD Ryzen 5 2600 Six-Core Processor) has x86-64-v3"))
        note = core.cpu_line({"v3": False, "name": "Intel(R) Celeron(R) N4120 CPU @ 1.10GHz"})
        self.assertTrue(note.startswith("note: the processor (Intel(R) Celeron(R) N4120 CPU @ 1.10GHz) has no x86-64-v3"))
        for words in ("the console prepares the shaders itself at its first start", "about an hour", "sets that up by itself"):
            self.assertIn(words, note)
        self.assertNotIn("--no-prepare", note)


def fake_build(tmp, **options):
    """A Builder after its step 6, with no game: the title and data folders as lay_out leaves them (the settings file and the
    skip list from data/), the shaders' lists in the work folder (no census of a game: bytes)"""
    out = os.path.join(tmp, "out")
    nup = os.path.join(tmp, "game.nup")
    os.makedirs(tmp, exist_ok=True)
    with open(nup, "wb") as f:
        f.write(b"NUP\x01" + bytes(60))
    b = core.Builder(core.Options(nup=nup, out=out, **options), Events())
    b.title = os.path.join(out, "homebrew", "GRPN00001")
    b.data_name = "grapnel"
    b.data = os.path.join(out, "data", b.data_name)
    os.makedirs(os.path.join(b.title, "sce_sys"))
    os.makedirs(b.data)
    shutil.copyfile(os.path.join(core.DATA, "hookshot_env.txt"), os.path.join(b.data, "hookshot_env.txt"))
    shutil.copyfile(os.path.join(core.DATA, "skip.txt"), os.path.join(b.data, "skip.txt"))
    with open(os.path.join(b.data, "image.bin"), "wb") as f:
        f.write(b"image")
    shaders = os.path.join(b.o.work, "shaders")
    os.makedirs(shaders)
    census, states = os.path.join(shaders, "hookshot_shaders.bin"), os.path.join(shaders, "hookshot_pipeline_states_shipped.bin")
    with open(census, "wb") as f:
        f.write(b"HKSEED01" + bytes(1000))
    with open(states, "wb") as f:
        f.write(b"states" * 100)
    b.shader_files = {"census": {"programs": 1}, "states": 1, "states_left_out": 0, "census_path": census, "states_path": states,
                      "states_sha256": core.sha256(states)}
    b.game_file, b.flat, b.files_found = nup, None, None
    b.seen, b.bid, b.game, b.icons_dir, b.art, b.tool_notes = {}, "0" * 16, None, None, [], {}
    return b


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class ConsolePreparation(unittest.TestCase):
    def test_setup_once(self):
        """The census and the list beside the game, HOOKSHOT_PS5_PREPARE=1 at the end of the settings file once, whatever the
        file's last line; a file that turned it off before gets it on again (its last line counts); force is kept"""
        with tempfile.TemporaryDirectory() as tmp:
            data = os.path.join(tmp, "grapnel")
            os.makedirs(data)
            census, states = os.path.join(tmp, "c.bin"), os.path.join(tmp, "s.bin")
            for p, blob in ((census, b"census"), (states, b"states")):
                with open(p, "wb") as f:
                    f.write(blob)
            env = os.path.join(data, "hookshot_env.txt")
            with open(env, "w", newline="\n") as f:
                f.write("A=1\n# HOOKSHOT_PS5_PREPARE=1 would ...\nB=2")   # (no newline at its end; the commented line not taken)
            self.assertFalse(core.console_prepare_ready(data))
            r = core.setup_console_prepare(data, census, states)
            self.assertEqual(r, {"files": ["hookshot_shaders.bin", "hookshot_pipeline_states_shipped.bin"], "setting_added": True})
            self.assertEqual(read(os.path.join(data, "hookshot_shaders.bin")), "census")
            self.assertEqual(read(os.path.join(data, "hookshot_pipeline_states_shipped.bin")), "states")
            text = read(env)
            self.assertTrue(text.startswith("A=1\n# HOOKSHOT_PS5_PREPARE=1 would ...\nB=2\n\n# (the builder"), text)
            self.assertTrue(text.endswith("\nHOOKSHOT_PS5_PREPARE=1\n"))
            self.assertTrue(core.console_prepare_ready(data))
            again = core.setup_console_prepare(data, census, states)   # (the window's button after the build: nothing added)
            self.assertFalse(again["setting_added"])
            self.assertEqual(read(env), text)
            self.assertEqual(sum(1 for l in text.splitlines() if l.strip() == "HOOKSHOT_PS5_PREPARE=1"), 1)
            self.assertFalse(any(name.endswith(".part") for name in os.listdir(data)))
            with open(env, "a") as f:
                f.write("HOOKSHOT_PS5_PREPARE=0\n")
            self.assertFalse(core.console_prepare_ready(data))
            self.assertTrue(core.setup_console_prepare(data, census, states)["setting_added"])
            self.assertEqual(core.env_setting(env, "HOOKSHOT_PS5_PREPARE"), "1")
            with open(env, "a") as f:
                f.write("HOOKSHOT_PS5_PREPARE=force\n")
            self.assertFalse(core.setup_console_prepare(data, census, states)["setting_added"])
            self.assertEqual(core.env_setting(env, "HOOKSHOT_PS5_PREPARE"), "force")
            os.remove(census)
            with self.assertRaises(core.BuildError) as e:
                core.setup_console_prepare(data, census, states)
            self.assertEqual(e.exception.code, "prepare-console")

    def test_the_shipped_settings_leave_it_off(self):
        """The builder's own settings file never turns the console's preparation on by itself: only step 7 does, on a PC
        that does not prepare"""
        self.assertIn(core.env_setting(os.path.join(core.DATA, "hookshot_env.txt"), core.CONSOLE_PREP_SETTING), (None, "", "0"))

    def test_check_on_a_pc_without_v3(self):
        """Step 3 on a processor without x86-64-v3: a note (not an error), and the console's preparation chosen; the PC's
        preparation tool is not even looked at (check() calls prepare_tool only without console_why)"""
        with tempfile.TemporaryDirectory() as tmp:
            for value, no_shaders, no_prepare, want in (("0", False, False, "cpu"), ("0", False, True, "cpu"),
                                                        ("0", True, False, None), ("1", False, False, None)):
                b = fake_build(os.path.join(tmp, "%s%d%d" % (value, no_shaders, no_prepare)), no_shaders=no_shaders,
                               no_prepare=no_prepare)
                with Forced(value):
                    b._check_cpu()
                self.assertEqual(b.console_why, want, (value, no_shaders, no_prepare))
                self.assertTrue(b.ev.infos[0].startswith("note: the processor" if value == "0" else "ok: the processor"))
                self.assertIn("%s=%s" % (core.CPU_V3_ENV, value), b.ev.logs[0])   # (what was read, in the log)
        src = read(os.path.join(HERE, "grapnel_core.py"))
        check = src[src.index("    def check(self):"):src.index("    def _check_cpu(self):")]
        self.assertLess(check.index("self._check_cpu()"), check.index("prepare_tool(t)"))
        self.assertIn("if not (self.o.no_shaders or self.o.no_prepare or self.console_why):", check)

    def test_step7_sets_up_the_console_and_install_says_so(self):
        with tempfile.TemporaryDirectory() as tmp, Forced("0"):
            b = fake_build(tmp, no_prepare=True)   # (the window passes --no-prepare on such a PC)
            b._check_cpu()
            self.assertIsNone(b.prepare())
            self.assertIsNone(b.prepared)
            self.assertEqual(b.console_prep["why"], "cpu")
            self.assertTrue(core.console_prepare_ready(b.data))
            self.assertTrue(any(i.startswith("the console prepares the shaders itself at its first start") for i in b.ev.infos))
            b.prepare()   # (again: the line not added twice)
            env = read(os.path.join(b.data, "hookshot_env.txt"))
            self.assertEqual(sum(1 for l in env.splitlines() if l.strip() == "HOOKSHOT_PS5_PREPARE=1"), 1)
            self.assertFalse(os.path.exists(os.path.join(b.title, "radv-shader-cache")))
            m = b.write_notes()
            self.assertEqual(m["console_prepare"]["why"], "cpu")
            self.assertIsNone(m["prepared"])
            self.assertFalse(m["processor"]["x86_64_v3"])
            self.assertIn(os.path.join("data", "grapnel", "hookshot_shaders.bin"), m["files"])
            self.assertIn(os.path.join("data", "grapnel", "hookshot_pipeline_states_shipped.bin"), m["files"])
            en, fr = read(os.path.join(b.o.out, "INSTALL.en.txt")), read(os.path.join(b.o.out, "INSTALL.fr.txt"))
            for words in ("The console prepares the game's shaders itself, at its first start", "no AVX2 (x86-64-v3)",
                          "about an hour", "Prepare the shaders", "hookshot_shaders.bin", "hookshot_pipeline_states_shipped.bin",
                          "HOOKSHOT_PS5_PREPARE=1", "(image.bin, hookshot_env.txt, skip.txt, hookshot_shaders.bin, "
                          "hookshot_pipeline_states_shipped.bin)",
                          "image.bin, hookshot_shaders.bin and hookshot_pipeline_states_shipped.bin\n   into /data/hookshot",
                          "keep that folder when you copy a new build"):
                self.assertIn(words, en)
            for words in ("La console prépare elle-même les shaders du jeu, à son premier lancement", "n'a pas l'AVX2",
                          "environ une heure", "Préparer les", "hookshot_shaders.bin", "HOOKSHOT_PS5_PREPARE=1"):
                self.assertIn(words, fr)
            self.assertNotIn("compile the game's shaders as you play", en)   # (not the no-preparation text)
            self.assertNotIn("are prepared already", en)
            self.assertIn("data/grapnel/hookshot_shaders.bin", read(os.path.join(b.o.out, "PRIVATE.txt")))
            with open(os.path.join(b.o.out, "manifest.json"), encoding="utf-8") as f:
                self.assertEqual(json.load(f)["console_prepare"]["files"], ["hookshot_shaders.bin", "hookshot_pipeline_states_shipped.bin"])

    def test_a_pc_with_v3_and_no_prepare_sets_up_nothing(self):
        """--no-prepare on a PC that runs the preparation keeps its meaning: the shaders compiled in play, nothing beside the
        game (the window's "build only", whose shaders page then prepares on the PC)"""
        with tempfile.TemporaryDirectory() as tmp, Forced("1"):
            b = fake_build(tmp, no_prepare=True)
            b._check_cpu()
            self.assertIsNone(b.prepare())
            self.assertIsNone(b.console_prep)
            self.assertFalse(core.console_prepare_ready(b.data))
            self.assertFalse(os.path.exists(os.path.join(b.data, "hookshot_shaders.bin")))
            en = read(os.path.join(b.o.out, "INSTALL.en.txt")) if b.write_notes() else ""
            self.assertIn("compile the game's shaders as you play", en)

    def test_an_illegal_instruction_goes_to_the_console(self):
        """A run that still stops on an instruction the processor does not have (the check said x86-64-v3): said as a note,
        and the console's own preparation set up as on a PC without it; any other failure stays one"""
        with tempfile.TemporaryDirectory() as tmp, Forced("1"):
            b = fake_build(tmp)
            b._check_cpu()
            b.prep_tool = os.path.join(tmp, "prepare")

            def stopped(*a):
                b._prepare_runs("run", {}, "log", "the preparation", 0.0, 1.0)
            b._prepare_on_pc = stopped
            ill = 0xC000001D if os.name == "nt" else -4

            def run(*a, **k):
                raise core.BuildError("tool-failed", "the preparation failed (exit %d)" % ill)
            b._run = run
            self.assertIsNone(b.prepare())
            self.assertEqual(b.console_prep["why"], "illegal-instruction")
            self.assertTrue(core.console_prepare_ready(b.data))
            note = [i for i in b.ev.infos if i.startswith("note: the preparation on this PC stopped")]
            self.assertTrue(note, b.ev.infos)
            self.assertIn("console prepares the shaders itself at its first start", note[0])
            self.assertNotIn("--no-prepare", note[0])
            en = read(os.path.join(b.o.out, "INSTALL.en.txt")) if b.write_notes() else ""
            self.assertIn("The preparation on this PC stopped on an instruction its processor does not have.", en)
            self.assertIn("La préparation sur ce PC s'est arrêtée sur une instruction que son processeur n'a pas.",
                          read(os.path.join(b.o.out, "INSTALL.fr.txt")))

            b2 = fake_build(os.path.join(tmp, "other"))
            b2._check_cpu()
            b2.prep_tool = b.prep_tool

            def failed(*a):
                raise core.BuildError("prepare-failed", "the preparation on this PC ended (exit 3)")
            b2._prepare_on_pc = failed
            with self.assertRaises(core.BuildError) as e:
                b2.prepare()
            self.assertEqual(e.exception.code, "prepare-failed")
            self.assertIsNone(b2.console_prep)


class Window(unittest.TestCase):
    def gui(self):
        try:
            import grapnel_gui
        except ImportError:   # (no Tkinter here)
            self.skipTest("no tkinter")
        return grapnel_gui

    def test_the_check_line_in_both_languages(self):
        gui = self.gui()
        self.assertEqual((gui.T["fr"]["rows"]["cpu"], gui.T["en"]["rows"]["cpu"]), ("Processeur", "Processor"))
        self.assertEqual((gui.T["fr"]["cpu_ok"], gui.T["en"]["cpu_ok"]), ("ok", "ok"))
        rows = gui.Assistant.ROWS
        self.assertLess(rows.index("compile"), rows.index("cpu"))
        self.assertEqual(rows.index("cpu") + 1, rows.index("prepare"))
        for words in ("sans AVX2", "la console", "premier lancement", "environ une heure"):
            self.assertIn(words, gui.T["fr"]["cpu_console"])
        for words in ("no AVX2", "the console", "first start", "about an hour"):
            self.assertIn(words, gui.T["en"]["cpu_console"])
        for lang, words in (("fr", ("AVX2", "premier lancement", "environ une heure", "data\\grapnel")),
                            ("en", ("AVX2", "first start", "about an hour", "data\\grapnel"))):
            for w in words:
                self.assertIn(w, gui.T[lang]["shaders_cpu"], (lang, w))
            self.assertIn("AVX2" if lang == "fr" else "AVX2", gui.T[lang]["prep_cpu"])
        # (the out of date texts: the preparation exists for Windows since 0.3; prepare-cpu no longer sends to another PC)
        for lang in ("fr", "en"):
            self.assertNotIn("shaders_windows", gui.T[lang])
            self.assertNotIn("prep_windows", gui.T[lang])
            for k, v in gui.T[lang].items():
                if isinstance(v, str):
                    self.assertNotIn("que pour Linux", v, k)
                    self.assertNotIn("for Linux only", v, k)
        self.assertIn("premier lancement", gui.T["fr"]["errors"]["prepare-cpu"])
        self.assertNotIn("autre PC", gui.T["fr"]["errors"]["prepare-cpu"])

    def test_the_window_reads_the_processor(self):
        gui = self.gui()
        for value, want in (("0", False), ("1", True)):
            with Forced(value):
                self.assertEqual(gui.this_cpu()["v3"], want)
        src = read(os.path.join(HERE, "grapnel_gui.py"))
        self.assertIn('self.can_prepare = self.tool_ok and self.cpu["v3"]', src)
        self.assertIn('self.set_row("cpu", "info", t["cpu_console"])', src)
        self.assertIn("grapnel_core.setup_console_prepare(", src)   # (the fallback button: the core's, not its own copy)
        self.assertNotIn('"\\nHOOKSHOT_PS5_PREPARE=1"', src)

    def test_built_says_the_consoles_preparation(self):
        gui = self.gui()
        with tempfile.TemporaryDirectory() as out:
            with open(os.path.join(out, "manifest.json"), "w") as f:
                json.dump({"checkout": "v9 (source 1)", "files": {}, "prepared": None,
                           "console_prepare": {"why": "cpu", "files": ["hookshot_shaders.bin"]}}, f)
            old = gui.this_version
            gui.this_version = lambda: "v9"
            try:
                b = gui.built(out)
            finally:
                gui.this_version = old
            self.assertEqual((b["shaders"], b["console"]["why"]), (None, "cpu"))


if __name__ == "__main__":
    unittest.main()
