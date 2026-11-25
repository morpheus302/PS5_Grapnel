"""A dump's corrupted and missing files found at step 1 from its TOC alone (scripts/grapnel-builder/grapnel_game_files.py).

A player's 23 GB dump (25/11/2026) held 9 files its dumper read wrong (blocks of zeros: the Water Temple's gate texture, terrain
lighting, two voices, a setting file all zeros), their records' SHA-256 made from the zeros; the TOC's SHA-256 is the file's
content (codec 1 included). Compared with a sound dump's reference, they differ, and the 186,579 others agree. Here, with small
packages (tests/test_grapnel_flat.py's) and a reference made from the sound one:
  - the reference holds paths and hash prefixes only (no game content), and the sound dump agrees with it;
  - the same game stored with codec 1 agrees too (the TOC's hash is the content's, whatever the dumper compressed);
  - zeroed files (records made from the zeros), a missing file are said; a file more is not (other dumpers keep a few more);
  - a reference of another version says nothing;
  - step 1 (check_nup's facts, Builder.flatten) says "==== Corrupted files detected ====" first, with the files and what they
    are, before "Compression detected"; the command line's first line is it; manifest.json counts them; a sound dump: nothing;
  - the window has "Corrupted files detected" / "Fichiers corrompus détectés" in bold in a box on the game's page and the
    compression's page, the files with what they are, the effect and the advice;
  - the repository's reference: the supported version, 186,588 files, 8-byte hashes, the files the player's dump had wrong
    named as what they are.
Without the change (no grapnel_game_files.py) this fails.

    python -I -B tests/test_grapnel_game_files.py      (zstandard: hookshot-ps5/.venv/bin/python3 here)
"""
import lzma
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_MODULES = ["zstandard"]

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / "scripts" / "grapnel-builder"
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import grapnel_game_files as gf  # noqa: E402
import test_grapnel_flat as tf  # noqa: E402   (its small packages: make_nup, tree, GAME)


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def found_in(path, reference):
    import nup
    p = nup.Nup(str(path))
    try:
        return gf.check(p, nup.pick_title(p, None), str(reference))
    finally:
        p.f.close()


def package(path, files):
    tf.make_nup(path, [dict(id=tf.BASE_ID, version=tf.VERSION, meta=b"META", files=files)])


def main():
    with tempfile.TemporaryDirectory(prefix="grapnel-files-") as tmp:
        tmp = Path(tmp)
        sound = tmp / "sound.nup"
        package(sound, tf.tree("raw"))
        ref = tmp / "ref.xz"
        head = gf.make(str(sound), str(ref), verify=True)
        check(head["files"] == len(tf.GAME) and head["hash_bytes"] == 8 and head["verified"], "the reference: %s" % head)
        blob = lzma.decompress(ref.read_bytes())   # (paths and hash prefixes only: its exact length says so)
        first = blob.index(b"\n")
        paths = sorted(tf.GAME)   # (exefs/..., romfs/...: the reference's own paths)
        check(len(blob) == first + 1 + sum(len(p.encode()) + 1 for p in paths) + 8 * len(paths),
              "the reference holds more than the paths and the hashes' prefixes")
        check(json.loads(blob[:first])["format"] == gf.FORMAT, "its first line")
        f = found_in(sound, ref)
        check(f["reference"] and f["checked"] == len(tf.GAME) and not f["corrupted"] and not f["missing"] and not f["unknown"],
              "the sound dump against its own reference: %s" % f)
        # the same game stored with codec 1 (a fifth of the files, as those dumpers): its TOC's hashes are the content's
        packed = tmp / "codec1.nup"
        package(packed, tf.tree("codec1"))
        f = found_in(packed, ref)
        check(not f["corrupted"] and not f["missing"], "the codec 1 dump of the same game is said corrupted: %s" % f)
        # what a dumper read wrong (records made from the zeros, as the player's), a file left out, one more
        files = tf.tree("codec1")
        zs = tf.GAME["romfs/Pack/Actor/Link.pack.zs"]
        files["romfs/Pack/Actor/Link.pack.zs"] = tf.as_codec1(bytes(4096) + zs[4096:])
        voice = tf.GAME["romfs/Sound/Resource/Stream/Voice.bwav"]
        files["romfs/Sound/Resource/Stream/Voice.bwav"] = tf.raw(voice[:600] + bytes(len(voice) - 600))
        files["romfs/Misc/00/f000.dat"] = tf.raw(bytes(len(tf.GAME["romfs/Misc/00/f000.dat"])))
        del files["romfs/AI/x.ainb"]
        files["romfs/Event/Extra/More.bgyml"] = tf.raw(b"YB" + bytes(100))
        bad = tmp / "bad.nup"
        package(bad, files)
        f = found_in(bad, ref)
        check(f["corrupted"] == ["romfs/Misc/00/f000.dat", "romfs/Pack/Actor/Link.pack.zs", "romfs/Sound/Resource/Stream/Voice.bwav"],
              "the corrupted files: %s" % f["corrupted"])
        check(f["missing"] == ["romfs/AI/x.ainb"] and f["unknown"] == 1, "missing %s, not in the reference %d" % (f["missing"], f["unknown"]))
        check([gf.kind_of(x) for x in f["corrupted"] + f["missing"]] == ["other", "pack", "sound", "ai"], "the kinds")
        lines = gf.lines(f)
        check(len(lines) == 4 and "corrupted: romfs/Pack/Actor/Link.pack.zs (objects' pack)" in lines[1], "the lines: %s" % lines)
        # a reference of another version says nothing
        other = tmp / "other.nup"
        tf.make_nup(other, [dict(id=tf.BASE_ID, version=65536, meta=b"META", files=tf.tree("raw"))])
        check(found_in(other, ref)["reference"] is None, "a reference of another version spoke")
        # step 1, the command line, manifest.json
        os.environ["GRAPNEL_GAME_FILES_REFERENCE"] = str(ref)
        import grapnel_core as core
        facts = {}
        core.check_nup(str(bad), facts)
        check(facts["game_files"]["corrupted"] == f["corrupted"] and facts["flat"], "check_nup's facts: %s" % facts.get("game_files"))
        said = []
        ev = core.Events()
        ev.info = said.append
        b = core.Builder(core.Options(nup=str(bad), out=str(tmp / "out")), ev)
        b.flatten()
        check(said[0] == core.CORRUPT_BANNER == "==== Corrupted files detected ====", "the first line: %s" % said[:2])
        check(said.index(core.FLAT_BANNER) > said.index(core.CORRUPT_BANNER) + 4, "Compression detected before the list")
        check(any("Link.pack.zs (objects' pack)" in s for s in said) and any("Dump the game again" in s for s in said),
              "the list or the advice: %s" % said[:8])
        note = b.files_note()
        check(note["corrupted"] == 3 and note["missing"] == 1 and note["not_in_reference"] == 1, "manifest.json's: %s" % note)
        said.clear()
        core.Builder(core.Options(nup=str(sound), out=str(tmp / "out2")), ev).flatten()
        check(core.CORRUPT_BANNER not in said and any(s.startswith("ok: the .nup's") for s in said), "a sound dump: %s" % said)
        r = subprocess.run([sys.executable, "-I", str(HERE / "grapnel_build.py"), "--flatten", "--nup", str(bad), "--flat-dir",
                            str(tmp / "cli")], capture_output=True, text=True, env=dict(os.environ))
        out = r.stdout.splitlines()
        check(r.returncode == 0 and out[0] == "==== Corrupted files detected ====" and "Voice.bwav (sound)" in r.stdout,
              "the command line: %s" % r.stdout[:800])
        # the window: the box, in both languages, on the game's page and the compression's
        src = (HERE / "grapnel_gui.py").read_text(encoding="utf-8")
        check('"corrupt_banner": "Fichiers corrompus détectés"' in src and '"corrupt_banner": "Corrupted files detected"' in src,
              "the window's banner in both languages")
        box = src[src.index("    def corrupt_box("):src.index("    def pick_nup(")]
        check('t["corrupt_banner"]' in box and "bold" in box and 't["corrupt_effect"]' in box and "kind_of" in box,
              "the box: the banner in bold, the files with what they are, the effect and the advice")
        paint = src[src.index("    def paint_nup(self):"):src.index("    def corrupt_box(")]
        flat = src[src.index("    def page_flat(self):"):src.index("    def pick_flat_dir(self):")]
        check("self.corrupt_box(self.nup_damage" in paint and "self.corrupt_box(self.flat_damage" in flat, "the box on both pages")
        # the repository's reference
        head, real = gf.load(gf.REFERENCE)
        check(head and head["title"].endswith(" v131072") and head["files"] == len(real) == 186588 and head["hash_bytes"] == 8,
              "the repository's reference: %s" % head)
        check("romfs/Pack/ZsDic.pack.zs" in real and "exefs/main" in real and os.path.getsize(gf.REFERENCE) < 2_500_000,
              "the repository's reference's files or size")
        wrong = {"romfs/TerrainArc/HyruleField_02/3500000241.adult.bake_gi.extm": "terrain",
                 "romfs/Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs": "texture",
                 "romfs/Voice/Resource/Stream/EUde_EventFlowMsg-_Dm_TimeTemple_0050--Dm_TimeTemple_0050_Text_001_b.bwav": "voice",
                 "romfs/gsys/EnvObjSetMgrParam/Artwork.gsys__EnvObjSetMgrParam.bgyml": "setting"}
        for path, kind in wrong.items():
            check(path in real and gf.kind_of(path) == kind, "%s: %s" % (path, gf.kind_of(path)))
    print("PASS: corrupted and missing files from the TOC against a sound dump's reference (codec 1 agrees, a file more is not a "
          "fault); said first on step 1, the command line, manifest.json and the window, both languages")


if __name__ == "__main__":
    main()
