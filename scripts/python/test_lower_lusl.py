"""Tests of the ludus_compile_shader LUSL lowering helper (cmake/shaders/lower_lusl.py)."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

HELPER = Path(__file__).resolve().parents[2] / 'cmake' / 'shaders' / 'lower_lusl.py'
SPECIFICATION = importlib.util.spec_from_file_location('lower_lusl', HELPER)
lower_lusl = importlib.util.module_from_spec(SPECIFICATION)
SPECIFICATION.loader.exec_module(lower_lusl)


class LowerLuslTests(unittest.TestCase):
    def test_constant_names(self):
        self.assertEqual(lower_lusl.constant_name('imageSampler'), 'IMAGE_SAMPLER')
        self.assertEqual(lower_lusl.constant_name('input.PositionUv'), 'INPUT_POSITION_UV')
        self.assertEqual(lower_lusl.constant_name('shadowMap2'), 'SHADOW_MAP2')

    def test_slot_header_names_every_slot_and_location(self):
        header = lower_lusl.slot_header('composite', 'binding\tmain.lusl:1\timage\tTexture2D\t0\n'
                                                     'binding\tmain.lusl:2\timageSampler\tSamplerState\t1\n'
                                                     'vertex-input\tmain.lusl:9\tinput.PositionUv\t0\n'
                                                     'specialization\tmain.lusl:4\tShade<A>\t_luslSpecialization0_Shade\n')
        self.assertIn('namespace ludus::shaders::composite::slots', header)
        self.assertIn('inline constexpr foundation::uint32 IMAGE = 0;', header)
        self.assertIn('inline constexpr foundation::uint32 IMAGE_SAMPLER = 1;', header)
        self.assertIn('inline constexpr foundation::uint32 INPUT_POSITION_UV = 0;', header)
        self.assertLess(header.index('IMAGE = 0'), header.index('IMAGE_SAMPLER = 1'))

    def test_slot_header_reports_descriptor_array_count(self):
        header = lower_lusl.slot_header('sample', 'binding\tmain.lusl:1\timages\tTextureCube\t0\t2\n'
                                                  'binding\tmain.lusl:2\tsampler\tSamplerState\t2\n')
        self.assertIn('TextureCube `images` (2 descriptors).', header)
        self.assertIn('IMAGES = 0;', header)
        self.assertIn('SAMPLER = 2;', header)

    def test_conflicting_vertex_input_locations_fail(self):
        with self.assertRaises(SystemExit):
            lower_lusl.slot_header('composite', 'vertex-input\tmain.lusl:1\tuv\t0\n'
                                                'vertex-input\tmain.lusl:5\tuv\t1\n')

    def test_dependencies_follow_includes_and_include_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'shaders').mkdir()
            (root / 'sdk').mkdir()
            (root / 'shaders' / 'main.lusl').write_text('#include "common.luslh"\n#include "math.luslh"\n')
            (root / 'shaders' / 'common.luslh').write_text('extern slang "helpers.slang"\n{\n};\n')
            (root / 'shaders' / 'helpers.slang').write_text('')
            (root / 'sdk' / 'math.luslh').write_text('')
            (root / 'shaders' / 'unused.luslh').write_text('')
            snapshot = root / 'snapshot'
            directories, files = lower_lusl.stage_snapshot(root, 'shaders/main.lusl', ['sdk'], snapshot)
            self.assertEqual(directories, ['sdk'])
            self.assertEqual(sorted(files),
                             ['sdk/math.luslh', 'shaders/common.luslh', 'shaders/helpers.slang', 'shaders/main.lusl'])

    def test_external_roots_keep_order_and_remap_the_emitted_slang_include(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            source, first, second, output = (base / name for name in ('shaders', 'first root', 'second', 'build'))
            for folder in (source, first, second, output):
                folder.mkdir()
            (source / 'main.lusl').write_text('#include "library.luslh"\n')
            (first / 'library.luslh').write_text('extern slang "implementation.slang" {};\n')
            (first / 'implementation.slang').write_text('// shared implementation\n')
            (second / 'library.luslh').write_text('// shadowed library\n')
            snapshot = base / 'snapshot'
            directories, files = lower_lusl.stage_snapshot(source, 'main.lusl', [first, second], snapshot)
            self.assertEqual([files[f'{root}/library.luslh'].parent for root in directories], [first, second])
            logical_include = f'{directories[0]}/implementation.slang'
            self.assertEqual((snapshot / logical_include).read_text(), '// shared implementation\n')
            self.assertEqual(lower_lusl.remap_slang_includes(f'#include "{logical_include}"\n', files, output),
                             '#include "../first root/implementation.slang"\n')


if __name__ == '__main__':
    unittest.main()
