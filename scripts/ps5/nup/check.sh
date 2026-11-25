#!/usr/bin/env bash
# The .nup RomFS layer (host/nup_fs.c) checked on this PC, no console, no game code (scripts/ps5/nup):
#   check.sh build                         the two checks, into out-ps5/nup/
#   check.sh layer <file.nup> <romfs>      nup_check: every file and listing of the package against the extracted folder
#   check.sh nnfs <file.nup> <romfs>       nnfs_check: host/nn_fs.c's own entry points over both, mods included
#   check.sh hashlist <file.nup> <romfs> <out>   the platform probe's list (game-derived: keep it in hookshot-ps5/)
#   check.sh probe <file.nup> <list>       the platform probe's own nup check (plat-probe/probe_nup.c) on this PC
#   check.sh same <reference.nup> <other.nup>   nup_same: two packages read through the layer, file by file (a dump and the
#                                          builder's flat copy of another: scripts/grapnel-builder/grapnel_flat.py)
# nnfs_check compiles host/nn_fs.c, mods.c, nup_fs.c and posix/win32_posix.c as the Linux build does; the guest model's
# headers come from a build's work folder: RT64 (work/sa/build-ps5/rt64) and GEN (work/sa/gen), by default wt/ps5-port's.
# THREADS (4), nice 10.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out=$root/out-ps5/nup
mkdir -p "$out"
[[ -f $root/out-ps5/.gitignore ]] || echo '*' > "$root/out-ps5/.gitignore"
work=${WORK_SA:-$root/../ps5-port/work/sa}
rt64=${RT64:-$work/build-ps5/rt64}
gen=${GEN:-$work/gen}
cc=${CC:-clang}
build() {
    nice -n 10 "$cc" -O2 -g -Wall -Wextra -D_GNU_SOURCE -I"$root/host" "$root/scripts/ps5/nup/nup_check.c" "$root/host/nup_fs.c" \
        -lpthread -o "$out/nup_check"
    local fl=(-O2 -g -D_GNU_SOURCE -DA64_POLLING -DA64_WRITE_TRACK -mcx16 -march=x86-64-v3 -I"$root/host/posix" -I"$root/host"
              -I"$rt64" -I"$gen" -Wno-everything)
    local objs=()
    for s in host/nn_fs.c host/mods.c host/nup_fs.c host/posix/win32_posix.c scripts/ps5/nup/nnfs_check.c; do
        nice -n 10 "$cc" "${fl[@]}" -c "$root/$s" -o "$out/$(basename "$s" .c).o"
        objs+=("$out/$(basename "$s" .c).o")
    done
    nice -n 10 "$cc" "${objs[@]}" -lpthread -ldl -lm -o "$out/nnfs_check"
    nice -n 10 "$cc" -O2 -g -Wall -Wextra -D_GNU_SOURCE -I"$root/host" -I"$root/scripts/ps5/plat-probe" "$root/scripts/ps5/nup/probe_nup_pc.c" \
        "$root/scripts/ps5/plat-probe/probe_nup.c" "$root/host/nup_fs.c" -lpthread -o "$out/probe_nup_pc"
    build_same
}
build_same() {   # (nup_same alone: no build's work folder needed)
    nice -n 10 "$cc" -O2 -g -Wall -Wextra -D_GNU_SOURCE -I"$root/host" "$root/scripts/ps5/nup/nup_same.c" "$root/host/nup_fs.c" \
        -lpthread -o "$out/nup_same"
}
export TMPDIR=${TMPDIR_NNFS:-$out}   # (nnfs_check's test mod: made and removed there)
case ${1:-build} in
    build) build ;;
    layer) build; nice -n 10 "$out/nup_check" "$2" "$3" --threads "${THREADS:-4}" --verify-toc ;;
    nnfs) build; nice -n 10 "$out/nnfs_check" "$2" "$3" --threads "${THREADS:-4}" ;;
    hashlist) build; nice -n 10 "$out/nup_check" --hashlist "$2" "$3" "$4" ;;
    probe) build; nice -n 10 "$out/probe_nup_pc" "$2" "$3" "${THREADS:-4}" "${SECONDS_NUP:-5}" ;;
    same) build_same; nice -n 10 "$out/nup_same" "$2" "$3" --threads "${THREADS:-4}" ;;
    *) sed -n '2,10p' "$0"; exit 2 ;;
esac
