"""Exercise seed_variant_of module ownership without a GPU or game."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

# (lot 87) the lazy shader modules' calls the cut functions make (gpu/vk/vk_progcache.cpp), off here
LAZY_STUBS = '\n// (lot 87: the lazy shader modules of gpu/vk/vk_progcache.cpp, off in this check: a module each, as before)\ninline bool lazy_modules_on() { return false; }\ninline VkShaderModule lazy_pack(Stage&, const std::vector<uint32_t>&, uint64_t) { return VK_NULL_HANDLE; }\ninline bool lazy_tagged(VkShaderModule) { return false; }\ninline void lazy_retire(Stage&) {}\n'

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
COMPILER = None
SOURCE = ROOT / 'gpu/vk/vk_shader.cpp'


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class SeedVariantCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = COMPILER or shutil.which('g++') or shutil.which('clang++') or shutil.which('cl')
        if not compiler:
            raise unittest.SkipTest('no C++20 compiler')
        cls.temporary = tempfile.TemporaryDirectory(prefix='longshot-seed-cleanup-')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name).resolve()
        header = (ROOT / 'gpu/vk/vk_shader.h').read_text(encoding='utf-8')
        structs = '\n'.join(block(header, marker) + ';' for marker in ('struct Stage {', 'struct Program {'))
        # (lot 87: Stage holds its packed SPIR-V, vk_progcache.cpp's lazy shader modules: declared, and off here)
        structs = 'struct PackedSpirv;\n' + structs + LAZY_STUBS
        (directory / 'seed_types.inc').write_text(structs, encoding='utf-8')
        source = SOURCE.read_text(encoding='utf-8')
        # (seed_variant_of hands its questions and answers to seed_variant_made, which seed_plain_of shares: both)
        markers = ['static const Program* seed_variant_made('] if 'seed_variant_made(' in source else []
        function = '\n\n'.join(block(source, m) for m in markers + ['const Program* seed_variant_of('])
        (directory / 'seed_function.inc').write_text(function, encoding='utf-8')
        cls.executable = directory / 'check.exe'
        fixture = ROOT / 'tests/seed_variant_cleanup_test.cpp'
        if Path(compiler).stem.lower() == 'cl':
            command = [compiler, '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX',
                       '/I' + str(directory), str(fixture), '/Fe:' + str(cls.executable)]
        else:
            command = [compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                       '-I' + str(directory), str(fixture), '-o', str(cls.executable)]
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_partial_graphics_failure_releases_modules(self):
        self.run_case('graphics_failure')

    def test_partial_mesh_failure_releases_modules(self):
        self.run_case('mesh_failure')

    def test_failure_with_only_null_modules(self):
        self.run_case('null_failure')

    def test_success_retains_modules_in_cache(self):
        self.run_case('success')

    def test_duplicate_destroys_only_temporary_modules(self):
        self.run_case('duplicate')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx')
    parser.add_argument('--source', type=Path, default=SOURCE)
    args, remaining = parser.parse_known_args()
    COMPILER, SOURCE = args.cxx, args.source
    unittest.main(argv=[__file__, *remaining])
