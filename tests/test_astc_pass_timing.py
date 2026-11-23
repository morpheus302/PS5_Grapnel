"""RX_VK_PASS_TIMING times the GPU's ASTC decode and BC re-encode, one span a texture (gpu/vk/vk_texture.cpp).

At an area's entry the GPU decodes dozens of ASTC textures in one frame, and re-encodes the big ones to BC3 (or BC7),
in the recordings' upload command buffers. That work showed in no span: the GPU sat at 100% for ~450 ms at Hyrule
Field's entry on an RX 9070 XT while the log's GPU time line showed 20 ms (#1720). Each texture's decode is now a span
of kind "astc", named by whether it was re-encoded.

(ps5 lot 60) Ported from #1843 onto the console's tree: its upload_defer has no rule of textures it never puts off, so
the labels say whether the texture is a layer array instead (the groups of levels of #1987 are a single-layer
texture's); the tree has no upload chunks, so the span is one scope, and the switch's row is in docs/development.md.

python -I -B tests/test_astc_pass_timing.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "gpu" / "vk" / "vk_texture.cpp"
DEV = ROOT / "docs" / "development.md"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def main():
    tex = TEX.read_text(encoding="utf-8")
    upload = block(tex, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {")
    span = "GprofSpan span(cmd, kSpans[tex.bc ? 1 : 0][tex.layers > 1 ? 1 : 0]);"
    need(span in upload and "span.bytes = staged;" in upload, "the ASTC decode has no RX_VK_PASS_TIMING span of its own")
    at = upload.index(span)
    need(upload.index("flush_mapped_range(s.allocation") < at, "the span opens inside the staging prefix")
    need(at < upload.index("vkCmdDispatch("), "a decode's dispatch runs before its span opens")
    need(at < upload.index("bc_scratch_layout(cmd"), "the BC scratch's layout change runs before the span opens")
    need("replay_flush_upload(" not in upload, "an upload chunk's flush in the decode: the span would count the CPU's wait")
    labels = block(upload, "static const char* const kSpans[2][2] = {")
    need(labels.count('"astc ') == 4, "a label of the ASTC spans does not start with the kind 'astc'")
    need(labels.count("BC re-encode") == 2 and labels.count("a layer array") == 2, "the four labels are not the four cases")

    dev = DEV.read_text(encoding="utf-8")
    row = next((line for line in dev.splitlines() if line.startswith("| `RX_VK_PASS_TIMING=1` or `2`")), "")
    need("ASTC" in row, "docs/development.md's RX_VK_PASS_TIMING row doesn't say it times the ASTC decode")
    print("PASS: RX_VK_PASS_TIMING times the ASTC decode and re-encode, one span a texture")


if __name__ == "__main__":
    main()
