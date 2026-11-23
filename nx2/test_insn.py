"""Every instruction form the game uses, in Unicorn and as lifted C: a64-kit's tests/insn_test.py with the Switch 2
additions.

Differences from running the vendored test as is:
  - the Switch 2 forms translate (insn.install()), and RETAx are returns (not sampled, like RET);
  - Unicorn runs its "max" CPU model, which has the ARMv8.1+ forms (the default Cortex-A72 rejects LSE, CAS, LDAPR);
  - the harness is built with -mcx16 (CASP on 128 bits);
  - PAC and MTE forms are left out: here they are the identity by design (see insn.py), not the hardware's result;
  - memory operands based on x29 / x30 get their scratch address too: capstone names them fp / lr, which
    insn_test.mem_regs does not parse, so those cases ran on a random (often tiny) address that the harness
    does not reset between cases (a false LDADD [x29] failure, 2026-10-31).

Usage:  python -I nx2/test_insn.py [--modules main,sdk,...] [--per 6] [--states 12] [--only lse]
  (--modules: every code module of the exefs but rtld by default)
  Needs the module images of nso.py in <work>/modules (made on first use).
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit
import insn

insn.install()
sys.path.insert(0, os.path.join(kit.KIT, "tests"))
import insn_test   # noqa: E402
import toolchain   # noqa: E402
import unicorn     # noqa: E402
from unicorn import arm64_const as A   # noqa: E402

# (the game: this runs the lifted code of a real dump against Unicorn)
RUN_ALL_SKIP = {
    "linux": "needs the game's modules (work/dump) and Unicorn",
    "windows": "needs the game's modules (work/dump) and Unicorn",
}


class _Unicorn:
    """insn_test's view of the unicorn module: every engine it makes runs the ARMv8.x "max" CPU"""
    UcError = unicorn.UcError
    UC_ARCH_ARM64 = unicorn.UC_ARCH_ARM64
    UC_MODE_ARM = unicorn.UC_MODE_ARM

    @staticmethod
    def Uc(arch, mode):
        uc = unicorn.Uc(arch, mode)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM64_MAX)
        return uc


ONLY = {"lse": lambda w: (w & 0x3F200C00) == 0x38200000 or (w & 0x3FA07C00) == 0x08A07C00
        or (w & 0xBFA07C00) == 0x08207C00,
        "nx2": lambda w: insn.emit(w) is not None,
        # (scalar FP and Advanced SIMD data processing: op0 x111)
        "fp": lambda w: (w >> 25) & 7 == 7,
        # (FMLA / FMLS / FMUL by element, vector and scalar: a64_lift.py vectorize_elem's)
        "elem": lambda w: (w >> 24) & 0x1F in (0x0F, 0x1F) and not (w >> 10) & 1 and (w >> 12) & 0xF in (1, 5, 9)}


class _LiftView:
    """(A64_TEST_OP_TEXT=1) insn_test's view of a64_lift: each instruction's C as the lift has it (op_text: FPX guards
    outlined, NEON FP vectorized, stores widened, A64_VFPX_ELEM's by-element ops), not outline_fpx's alone; with
    A64_TEST_HOT=1 also as a hot function has it (hot_text: the FPX helpers' inline copies, A64_FPX_NC's)"""
    def __init__(self, mod):
        self._mod = mod

    def __getattr__(self, name):
        return getattr(self._mod, name)

    counts = {"texts": 0, "by element, vectorized": 0, "hot copies": 0}

    def outline_fpx(self, text, used):
        t = self._mod.op_text(text, used)
        self.counts["texts"] += 1
        if os.environ.get("A64_TEST_HOT"):
            t = self._mod.hot_text(t)
            self.counts["hot copies"] += "a64_fpx_hot_" in t
        self.counts["by element, vectorized"] += ("a64_vfpx_fma32(c" in t) or (",_e,4); }" in t)   # (A64_VFPX_ELEM=hot: by hot_text)
        return t


def mem_regs(w):
    md = insn_test.capstone.Cs(insn_test.capstone.CS_ARCH_ARM64, insn_test.capstone.CS_MODE_ARM)
    md.detail = True
    ins = next(md.disasm(insn_test.struct.pack("<I", w), insn_test.CODE), None)
    alias = {"fp": 29, "lr": 30, "x29": 29, "x30": 30}
    for op in ins.operands if ins else ():
        if op.type == insn_test.capstone.arm64.ARM64_OP_MEM:
            def num(r):
                if r is None:
                    return None
                name = ins.reg_name(r)
                if name in ("sp", "wsp"):
                    return "sp"
                if name in alias:
                    return alias[name]
                return int(name[1:]) if name[1:].isdigit() else None
            return num(op.mem.base or None), num(op.mem.index or None)
    return None


def main():
    kit.wait_for_quiet('the instruction test')
    only = None
    if "--only" in sys.argv:
        i = sys.argv.index("--only")
        only = ONLY[sys.argv[i + 1]]
        del sys.argv[i:i + 2]
    modules = os.path.join(kit.WORK, "modules")
    if not os.path.exists(os.path.join(modules, "main.json")):
        subprocess.run([sys.executable, "-I", os.path.join(kit.KIT, "tools", "nso.py"), kit.need_exefs(), modules],
                       check=True, stdout=subprocess.DEVNULL)
    if "--modules" not in sys.argv:
        sys.argv += ["--modules", ",".join(kit.game_modules())]
    insn_test.unicorn = _Unicorn
    if os.environ.get("A64_TEST_OP_TEXT"):
        insn_test.a64_lift = _LiftView(insn_test.a64_lift)
        import atexit
        atexit.register(lambda: print("op_text: %s" % _LiftView.counts, flush=True))
    insn_test.mem_regs = mem_regs
    base_target = toolchain.native_target
    # (Linux: the harness's VirtualAlloc from nx2/test_shim/windows.h, the game-free harnesses' alone)
    shim = [] if os.name == "nt" or toolchain.mingw() else ["-I" + os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_shim")]
    toolchain.native_target = lambda *a, **k: base_target(*a, **k) + ["-mcx16"] + shim
    base_sample = insn_test.sample

    def sample(image_dir, mods, per, rng):
        picked = [(k, w) for k, w in base_sample(image_dir, mods, per, rng) if not insn.is_identity_model(w)]
        return [(k, w) for k, w in picked if only(w)] if only else picked

    insn_test.sample = sample
    sys.argv = [sys.argv[0], modules] + sys.argv[1:]
    return insn_test.main()


if __name__ == "__main__":
    sys.exit(main())
