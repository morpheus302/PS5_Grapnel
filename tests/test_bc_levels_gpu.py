"""The BC3 bytes of the ASTC-to-BC re-encode are the same, level for level, whatever way the levels and layers go (gpu/vk/vk_texture.cpp).

(ps5 lot 60) Longshot #1843 decodes an array's levels of 1024 or less 8 layers a run through a small scratch, #2015 gives the
copies out of the encoder's buffer their pitch, and #1987 decodes a single-layer texture's levels one into each of the small
scratch's layers and encodes and copies them as a group. Each says "same decoder, same encoder, same data": this check runs
it. tests/bc_levels_gpu.cpp is compiled around the production code, cut out of vk_texture.cpp (upload_astc_gpu,
bc_scratch_make, bc_scratch_layout, bc_encode_into, level_geo and the scratch's globals), once from this tree and once from
the tree before the three changes (lot 57, --before, a185d64a by default). Both run on a Vulkan device of this PC with the
renderer's own decoder and encoder, over synthetic ASTC textures in the Switch 2 layout (sizes of 4 to 2048, not multiples
of 4, block sizes 4x4 to 12x12, layer arrays), and the BC3 images read back must be the same bytes: the tree with its
defaults, with RX_VK_BC_LEVELS=0, RX_VK_BC_SMALL_LAYERS=1, RX_VK_BC_COPY_PITCH=0 and as a small-VRAM card (its 256 x 256
scratch), against lot 57's code. It also prints the full barriers each upload recorded.

(ps5 lot 61) RX_VK_BC_ONE_PASS=1 decodes all the levels of a single-layer texture in one dispatch and encodes them in one,
storing the blocks straight into the image through raw R32G32B32A32_UINT views of its levels (RX_VK_BC_ONE_PASS_COPY=1: through
the encoder's buffer and one copy): both are checked against lot 57's bytes the same way, with the decoder and encoder the tree
compiles from astc_decoder.comp and bc_encoder.comp under their defines. --time N also prints the GPU time of an upload, N in a
command buffer (before's, lot 60's and the one pass's; with TEST_SYNC_CS=1 too, an execution barrier after every dispatch as
the console's driver waits for each, RADV_PS5_SYNC_CS=1): a measure of this PC's GPU, not of the console's.

It needs a GPU, so tests/run_all.py leaves it out (RUN_ALL_SKIP); run it by hand:

    python -I -B tests/test_bc_levels_gpu.py --headers <Vulkan-Headers/include> [--cxx g++] [--device NAME] [--before REV]
        [--time N]

--device picks a device by a part of its name (lavapipe: llvmpipe). The bytes say nothing of the barriers on a GPU that
happens to run the dispatches in order: they are upstream's, by reading. (The Khronos layer's synchronization validation,
Debian's 1.4.309, was tried on this harness and reported nothing even with a barrier taken out between the encodes and the
copy: it does not see these shaders' accesses, so it is not used here.)
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
RUN_ALL_SKIP = {"linux": "needs a GPU: run by hand", "windows": "needs a GPU and dlopen: run by hand on Linux"}

ROOT = Path(__file__).resolve().parents[1]
TEX = "gpu/vk/vk_texture.cpp"
HARNESS = ROOT / "tests" / "bc_levels_gpu.cpp"
ONE_PASS = "// ---- (ps5 lot 61) one pass for all the levels of a single-layer BC3 texture ----"
SHADERS = ROOT / "gpu" / "vk" / "host_shaders"


def need(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def production(tex):
    """The pieces of vk_texture.cpp the harness runs, in the order they are declared there"""
    parts = [block(tex, "struct BcPush {") + ";", block(tex, "struct AstcPush {") + ";",
             between(tex, "constexpr uint32_t kBcDim = 2048, kBcLayers = 8;", "bool bc7_enabled() {"),
             between(tex, "VkImage bc_scratch = VK_NULL_HANDLE;", "bool bc_scratch_make() {"),
             block(tex, "bool bc_scratch_make() {")]
    if "bool bc_copy_pitch() {" in tex:   # (lot 60's; before it, the copies left the pitch to the driver)
        parts.append(block(tex, "bool bc_copy_pitch() {"))
    one_pass = ONE_PASS in tex   # (lot 61's: its globals and switches, and the function that records it)
    if one_pass:
        parts.append(between(tex, ONE_PASS, "/// The one-pass pipelines, made at the first texture that asks"))
    parts += [block(tex, "void bc_encode_into(VkCommandBuffer cmd,"), block(tex, "bool bc_scratch_layout(VkCommandBuffer cmd"),
              block(tex, "struct LevelGeo {") + ";", block(tex, "LevelGeo level_geo(const Texture& tex, uint32_t level) {"),
              "using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;",
              block(tex, "bool overlaps(const Ranges* dirty, uint64_t a, uint64_t b) {")]
    if one_pass:
        parts.append(block(tex, "bool bc_one_pass(VkCommandBuffer cmd,"))
    parts.append(block(tex, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {"))
    return "\n\n".join(parts)


def build(cxx, headers, tex, exe):
    harness = HARNESS.read_text(encoding="utf-8")
    need(harness.count("// PRODUCTION_BC") == 1, "the harness lost its marker")
    decl = ("void bc_encode_resized(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, Texture&, uint32_t, "
            "uint32_t);\n")
    src = exe.with_suffix(".cpp")
    src.write_text(harness.replace("// PRODUCTION_BC", decl + production(tex)), encoding="utf-8")
    defines = ["-DONE_PASS_CUT"] if ONE_PASS in tex else []
    subprocess.run([cxx, "-std=c++20", "-O1", "-w", "-I" + str(headers), "-I" + str(SHADERS)] + defines + [str(src), "-o", str(exe),
                    "-ldl"], check=True)


def run(exe, out, env_extra, device, time_n=0):
    env = {k: v for k, v in os.environ.items()
           if k not in ("RX_VK_BC_LEVELS", "RX_VK_BC_SMALL_LAYERS", "RX_VK_BC_COPY_PITCH", "RX_VK_BC_LAYERS", "RX_VK_BC_POOL",
                        "TEST_LOW_VRAM", "RX_VK_BC_ONE_PASS", "RX_VK_BC_ONE_PASS_COPY", "TEST_SYNC_CS")}
    env.update(env_extra)
    cmd = [str(exe), "--out", str(out)] + (["--device", device] if device else []) + (["--time", str(time_n)] if time_n else [])
    r = subprocess.run(cmd, text=True, capture_output=True, timeout=600, env=env)
    if r.returncode or "DONE" not in r.stdout:
        if r.stdout.startswith("SKIP:"):
            return r
        raise SystemExit(f"FAIL: {' '.join(f'{k}={v}' for k, v in env_extra.items()) or 'defaults'}: exit {r.returncode}\n"
                         f"{r.stdout}{r.stderr}")
    return r


def textures(path):
    """{name: (header, bytes)} from a harness output"""
    data = path.read_bytes()
    out, at = {}, 0
    while at < len(data):
        nl = data.index(b"\n", at)
        header = data[at:nl].decode()
        size = int(header.rsplit(" ", 1)[1])
        out[header.split(" levels ")[0]] = (header, data[nl + 1:nl + 1 + size])
        at = nl + 1 + size
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--device")
    p.add_argument("--before", default="a185d64a", help="the commit whose code is the reference (lot 57)")
    p.add_argument("--time", type=int, default=0, help="also time N uploads of each of six textures on the GPU")
    a = p.parse_args()
    now = (ROOT / TEX).read_text(encoding="utf-8")
    before = subprocess.run(["git", "-C", str(ROOT), "show", f"{a.before}:{TEX}"], text=True, capture_output=True,
                            check=True).stdout
    need("bc_small_layer[" not in before, f"{a.before} already groups the levels: not the code before the changes")
    with tempfile.TemporaryDirectory(prefix="bc-levels-gpu-") as temp:
        t = Path(temp)
        build(a.cxx, a.headers, before, t / "before.exe")
        build(a.cxx, a.headers, now, t / "now.exe")
        ref = run(t / "before.exe", t / "before.bin", {}, a.device)
        if ref.stdout.startswith("SKIP:"):
            print(ref.stdout.strip())
            return
        print(f"before ({a.before}, one level and one layer a run):")
        print(ref.stdout.strip())
        reference = textures(t / "before.bin")
        need(len(reference) == 13, f"the reference has {len(reference)} textures, not 13")
        modes = [("lot 60, defaults (#1843 + #2015 + #1987)", {}),
                 ("RX_VK_BC_LEVELS=0", {"RX_VK_BC_LEVELS": "0"}),
                 ("RX_VK_BC_SMALL_LAYERS=1", {"RX_VK_BC_SMALL_LAYERS": "1"}),
                 ("RX_VK_BC_COPY_PITCH=0", {"RX_VK_BC_COPY_PITCH": "0"}),
                 ("a small-VRAM card (256 x 256 scratch)", {"TEST_LOW_VRAM": "1"}),
                 ("RX_VK_BC_SMALL_LAYERS=4", {"RX_VK_BC_SMALL_LAYERS": "4"})]
        if ONE_PASS in now:
            modes += [("lot 61, RX_VK_BC_ONE_PASS=1 (stored straight into the image)", {"RX_VK_BC_ONE_PASS": "1"}),
                      ("lot 61, RX_VK_BC_ONE_PASS=1 + RX_VK_BC_ONE_PASS_COPY=1 (through the buffer and one copy)",
                       {"RX_VK_BC_ONE_PASS": "1", "RX_VK_BC_ONE_PASS_COPY": "1"}),
                      ("lot 61, RX_VK_BC_ONE_PASS=1 + RX_VK_BC_SMALL_LAYERS=2 (the levels in two layers at most)",
                       {"RX_VK_BC_ONE_PASS": "1", "RX_VK_BC_SMALL_LAYERS": "2"}),
                      ("lot 61, RX_VK_BC_ONE_PASS=1 on a small-VRAM card (256 x 256 scratch)",
                       {"RX_VK_BC_ONE_PASS": "1", "TEST_LOW_VRAM": "1"})]
        for name, env in modes:
            r = run(t / "now.exe", t / "now.bin", env, a.device)
            got = textures(t / "now.bin")
            need(got.keys() == reference.keys(), f"{name}: other textures than the reference's")
            for tex, (header, data) in reference.items():
                need(got[tex][0] == header, f"{name}: {tex}: {got[tex][0]} against {header}")
                if got[tex][1] != data:
                    first = next(i for i in range(min(len(data), len(got[tex][1]))) if data[i] != got[tex][1][i])
                    raise SystemExit(f"FAIL: {name}: {tex}: the BC3 bytes differ from lot 57's code, first at byte {first}")
            digest = hashlib.sha256(t.joinpath("now.bin").read_bytes()).hexdigest()[:16]
            print(f"\n{name}: every texture's BC3 bytes the same as before's (sha256 {digest}):")
            print("\n".join(line for line in r.stdout.splitlines() if " levels x" in line))
            if env.get("RX_VK_BC_ONE_PASS") and not env.get("TEST_LOW_VRAM") and not env.get("RX_VK_BC_SMALL_LAYERS"):
                one = [line for line in r.stdout.splitlines() if " levels x1 " in line
                       and "2048x256" not in line and "1365x1365" not in line]   # (those two have a level over 1024)
                # (the single-layer textures that fit the small scratch: one decode and one encode each)
                need(one and all(" 1 decodes,    1 encodes" in line for line in one),
                     f"{name}: a single-layer texture that fits the small scratch went in more than two dispatches")
                if not env.get("RX_VK_BC_ONE_PASS_COPY"):
                    need(all("  0 copies" in line for line in one), f"{name}: a copy where the blocks go straight into the image")
        if a.time:
            print(f"\nGPU time of an upload on this PC ({a.time} in a command buffer; a measure of this GPU, not the console's):")
            timed = [("before (lot 57/59: a level a run)", t / "before.exe", {}), ("lot 60 (groups)", t / "now.exe", {})]
            if ONE_PASS in now:
                timed += [("lot 61, one pass, into the image", t / "now.exe", {"RX_VK_BC_ONE_PASS": "1"}),
                          ("lot 61, one pass, buffer and one copy", t / "now.exe",
                           {"RX_VK_BC_ONE_PASS": "1", "RX_VK_BC_ONE_PASS_COPY": "1"})]
            for sync in ("0", "1"):
                for name, exe, env in timed:
                    r = run(exe, t / "time.bin", dict(env, TEST_SYNC_CS=sync), a.device, a.time)
                    print(f"\n{name}{', TEST_SYNC_CS=1' if sync == '1' else ''}:")
                    print("\n".join(line for line in r.stdout.splitlines() if line.startswith("time ")))
    print("\nPASS: the BC3 bytes of every level and layer are lot 57's, with the groups, the 8-layer runs and the copies' pitch"
          + (", and with lot 61's one pass (into the image and through the buffer)" if ONE_PASS in now else ""))


if __name__ == "__main__":
    main()
