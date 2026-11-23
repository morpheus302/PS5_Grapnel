#!/usr/bin/env bash
# Hookshot on the PS5: the header-only packages the Vulkan back end compiles against, at the revisions
# scripts/linux/deps.sh pins (Vulkan-Headers, SPIRV-Headers, VulkanMemoryAllocator), in the CPM cache layout
# nx2/build.py's cpm_package() reads: third_party-ps5/cpm/<name>/<version>/. Header-only: nothing here is built
# for the host. Downloads are kept in third_party-ps5/dl.
#
#   scripts/ps5/fetch-headers.sh            (then RX_CPM=<worktree>/third_party-ps5/cpm)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
eval "$(grep -E '^(VK_HEADERS|SPIRV_HEADERS|VMA)=' "$root/scripts/linux/deps.sh")"   # (one set of pins for Linux and the PS5)
cpm=$root/third_party-ps5/cpm
dl=$root/third_party-ps5/dl
mkdir -p "$cpm" "$dl"
get() {   # repo tag name
    local dst=$cpm/$3/$2
    [[ -f $dst/.complete ]] && return 0
    [[ -f $dl/$3-$2.tar.gz ]] || curl -fsSL --retry 3 -o "$dl/$3-$2.tar.gz.part" "https://github.com/$1/archive/refs/tags/$2.tar.gz"
    [[ -f $dl/$3-$2.tar.gz.part ]] && mv "$dl/$3-$2.tar.gz.part" "$dl/$3-$2.tar.gz"
    rm -rf "$dst" && mkdir -p "$dst"
    tar -xzf "$dl/$3-$2.tar.gz" -C "$dst" --strip-components=1
    touch "$dst/.complete"
    echo "$3 $2: $(sha256sum "$dl/$3-$2.tar.gz" | cut -c1-16)"
}
get KhronosGroup/Vulkan-Headers "$VK_HEADERS" vulkanheaders
get KhronosGroup/SPIRV-Headers "$SPIRV_HEADERS" spirv-headers
get GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator "$VMA" vulkanmemoryallocator
echo "RX_CPM=$cpm"
