"""A render target's image whose texture is gone waits in a pool by its create info, and a new texture of that create info
takes it, instead of each of OoT's shifting transient targets being a new image made and destroyed.

OoT makes a frame's transient targets in a scratch heap whose offsets shift with what came before them in the frame, so each
shifted one is a texture of its own (vk_texture.cpp's dead targets). Every 60 presents the sweep let go of the dead ones,
300-700 at once, and the next present destroyed them all: 20-65 ms on the replaying thread, about once a second while the
camera moves. These checks keep the pool's rules: what is pooled, that a pooled image is still put into GENERAL from UNDEFINED
and cleared as a new one is, that the dead list pools before it destroys, and that the pool is bounded and can be turned off.

python -I -B tests/test_target_image_pool.py
"""
from pathlib import Path
import re
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


class TargetImagePoolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
        cls.hdr = (ROOT / "gpu" / "vk" / "vk_texture.h").read_text(encoding="utf-8")

    def test_the_key_is_the_whole_create_info(self):
        key = block(self.tex, "uint64_t pool_key_of(const VkImageCreateInfo& ci)")
        for field in ("ci.flags", "ci.imageType", "ci.format", "ci.extent.width", "ci.extent.height", "ci.extent.depth",
                      "ci.mipLevels", "ci.arrayLayers", "ci.samples", "ci.usage", "ci.tiling"):
            self.assertIn(field, key, field)
        self.assertIn("| 1", key)   # (never 0: 0 is "not pooled")

    def test_only_uncompressed_render_targets_are_pooled_and_zero_turns_it_off(self):
        make = block(self.tex, "Texture* create(const Key& key")
        rule = make[make.index("const uint64_t pool_key ="):make.index("x.pool_key = pool_key;")]
        for part in ("as_target", "!x.sparse", "pool_cap()", "!is_bc(x.format)", "!is_astc(x.format)",
                     "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT", "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT"):
            self.assertIn(part, rule, part)
        cap = block(self.tex, "uint64_t pool_cap()")
        self.assertIn('getenv("RX_VK_TARGET_POOL_MB")', cap)
        self.assertIn("target_pool::default_mb(vram_usage(&used, &allowed) ? allowed : 0)", cap)
        self.assertIn("uint64_t pool_key = 0;", self.hdr)

    def test_a_texture_made_for_a_target_is_one_and_its_image_comes_from_the_targets_blocks(self):
        get = block(self.tex, "Texture* texture_get(const nvn_tex* t, bool for_sampling)")
        self.assertIn("create(k, t, fi.format, fi.astc, bw, bh, bpb, !for_sampling)", get)
        make = block(self.tex, "Texture* create(const Key& key")
        self.assertIn("create_image(ci, x.image, x.allocation, false, as_target && !is_bc(x.format) && !is_astc(x.format))", make)
        vma = (ROOT / "gpu" / "vk" / "vk_vma.cpp").read_text(encoding="utf-8")
        image = block(vma, "bool create_image(const VkImageCreateInfo& ci")
        self.assertIn("target && target_images_pool() ? target_images_pool() : images_pool()", image)
        pool = block(vma, "VmaPool target_images_pool()")
        self.assertIn('getenv("RX_VK_TARGET_BLOCKS")', pool)
        self.assertIn("pi.blockSize = image_block();", pool)   # (ps5 port: the block size is read when the pool is made)
        self.assertIn("void vma_log_image_blocks()", vma)

    def test_the_dead_targets_are_swept_every_present_after_one_unused(self):
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        every = block(self.tex, "uint64_t dead_every_presents()")
        self.assertIn('getenv("RX_VK_DEAD_EVERY")', every)
        self.assertIn(": kDeadEveryDefault;", every[every.index('getenv("RX_VK_DEAD_EVERY")'):])
        self.assertIn("frame_no % (dead_targets ? dead_every : 600)", frame)
        unused = block(self.tex, "uint64_t dead_min_unused_presents()")
        self.assertIn(": kDeadMinUnusedDefault;", unused[unused.index('getenv("RX_VK_DEAD_MIN_UNUSED")'):])
        # (ps5 line: 1 and 1 off the console, upstream's; 10 and 3 on it, as before #994, unless set)
        self.assertIn("#else\nstatic constexpr uint64_t kDeadEveryDefault = 1, kDeadMinUnusedDefault = 1;", self.tex)
        # (and the pass that reads them is not the only one that looks: it runs at every tenth present, and on the presents
        # between the sweep does. tests/test_dead_targets_sweep.py has the presents)
        self.assertIn("dead_targets_sweep(completed);", frame)

    def test_a_pooled_image_is_taken_before_one_is_made_and_still_cleared_from_undefined(self):
        make = block(self.tex, "Texture* create(const Key& key")
        take = make.index("pool_take(pool_key, x.image, x.allocation, x.views)")
        self.assertLess(take, make.index("!create_image(ci, x.image, x.allocation, false, as_target"))
        after = make[take:]
        self.assertIn("b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;", after)
        self.assertIn("vkCmdClearColorImage", after)
        self.assertIn("vkCmdClearDepthStencilImage", after)

    def test_a_gone_render_targets_views_go_to_the_pool_with_its_image(self):
        later = block(self.tex, "void destroy_later(Texture& tex)")
        self.assertRegex(later, r"if \(tex\.pool_key && tex\.allocation && tex\.sparse_chunks\.empty\(\) && pool_cap\(\)\)")
        self.assertIn("d.view_map = std::move(tex.views)", later)
        self.assertIn("d.views.push_back(kv.second)", later)   # (the others' views destroyed, as before)

    def test_the_dead_list_pools_before_it_destroys_and_the_pool_is_trimmed_each_present(self):
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        # (the dead list's walk is reap_dead since ps5-next-22: the present calls it before the pool's trim, and the eviction
        # pass may call it again for what it let go, RX_VK_EVICT_FREE_NOW)
        reap = block(self.tex, "size_t reap_dead(uint64_t completed, uint64_t hold, uint64_t* bytes = nullptr)")
        loop = reap[reap.index("for (auto it = dead.begin(); it != dead.end();) {"):]
        self.assertLess(loop.index("if (it->pool_key) {"), loop.index("destroy_image(it->image, it->allocation);"))
        self.assertIn("pool_put(*it);", loop[:loop.index("destroy_image(")])
        trim = frame.index("pool_trim();")
        self.assertLess(frame.index("reap_dead(completed, hold);"), trim)
        # (every present, not every tenth: the line that returns on the other nine is "% 10 && !pressed" since ps5-next-23, and
        # tex_peaks::pass_now since ps5-next-37-pics, the same unless RX_VK_EVICT_BURST_EVERY is set; since lot 46 (#1255) the
        # dead targets' sweep runs there before it returns; since lot 63 keep_due, RX_VK_EVICT_KEEP_FREE_MB's, comes first)
        self.assertLess(trim, frame.index("if (!keep_due && !tex_peaks::pass_now(frame_no, pressed, burst, burst_every)) {"))
        self.assertIn("pool_pressed = known && pressure > allowed / 10 * 9;", frame)

    def test_the_pool_can_be_kept_while_vram_is_short(self):
        # (ps5) the console's driver always counts VRAM as short, so the pool was emptied every present: RX_VK_TARGET_POOL_KEEP=1
        # keeps it, still bounded by its cap and its 120 presents; unset, nothing changes
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        pressed = frame.index("pool_pressed = known && pressure > allowed / 10 * 9;")
        keep = frame.index('static const bool keep = getenv("RX_VK_TARGET_POOL_KEEP") && atoi(getenv("RX_VK_TARGET_POOL_KEEP")) > 0;')
        self.assertLess(pressed, keep)
        after = frame[keep:]
        self.assertLess(after.index("pool_short = keep && pool_pressed;"), after.index("if (keep) pool_pressed = false;"))
        self.assertIn("VRAM short: kept all the same (RX_VK_TARGET_POOL_KEEP)", frame)
        self.assertIn("VRAM short: given back", frame)
        # (the budget's own squeeze does not read the pool's flag: it is as before whatever the pool does)
        self.assertIn("if (known && pressure > allowed / 10 * 9) {", frame[keep:])
        trim = block(self.tex, "size_t pool_trim()")
        self.assertNotIn("RX_VK_TARGET_POOL_KEEP", trim)

    def test_the_trim_is_bounded_a_present_and_gives_back_idle_and_excess_images(self):
        trim = block(self.tex, "size_t pool_trim()")
        self.assertIn("pool_pressed ? 0 : pool_cap()", trim)
        # (since ps5-next-37-pics the bound a present is tex_peaks::trim_most: 64 while VRAM is short, 16 otherwise, more over the
        # pool's own bound only if RX_VK_TARGET_POOL_TRIM asks)
        self.assertIn("n < tex_peaks::trim_most(pool_pressed, pool_bytes > cap, pool_trim_most());", trim)
        # (since ps5-next-37-pics the rule is tex_peaks::pool_drop: over the cap, or unused more than the hold; tests/test_tex_peaks.py
        # checks the rule itself. Since lot 46 (#1255) the hold is RX_VK_POOL_IDLE first, RX_VK_TARGET_POOL_HOLD after it, and
        # unset 120 on the console (lot 44's), target_pool::kIdlePresents (8, upstream's) elsewhere)
        self.assertIn("if (!tex_peaks::pool_drop(pool_bytes, cap, frame_no, v[i].since, hold)) {", trim)
        self.assertIn("const uint64_t hold = pool_hold();", trim)
        hold = block(self.tex, "uint64_t pool_hold()")
        self.assertLess(hold.index('getenv("RX_VK_POOL_IDLE")'), hold.index('getenv("RX_VK_TARGET_POOL_HOLD")'))
        ps5 = hold[hold.index("#ifdef __PROSPERO__"):hold.index("#endif")]
        self.assertIn("return (uint64_t)120;", ps5)
        self.assertIn("return target_pool::kIdlePresents;", ps5[ps5.index("#else"):])
        rules = (Path(__file__).resolve().parents[1] / "gpu/vk/tex_peaks.h").read_text(encoding="utf-8")
        self.assertIn("return pool_bytes > cap || frame - since > hold;", rules)
        self.assertIn("pool_destroy(v[i]);", trim)
        destroy = block(self.tex, "void pool_destroy(Pooled& p)")
        self.assertIn("vkDestroyImageView", destroy)
        self.assertIn("destroy_image(p.image, p.allocation);", destroy)

    def test_the_pools_numbers_are_logged(self):
        self.assertIn("render target images: %llu reused from the pool, %llu made new, %llu destroyed from it", self.tex)


if __name__ == "__main__":
    unittest.main()
