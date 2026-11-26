"""Tests of the ludus_compile_shader LUSL lowering helper (cmake/shaders/lower_lusl.py)."""

import contextlib
import importlib.util
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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

    def test_glsl_es_sources_are_found_through_includes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'main.lusl').write_text('#include "fullscreen.luslh"\n')
            (root / 'fullscreen.luslh').write_text('#if LUDUS_GLSL_ES\n#endif\n')
            (root / 'plain.lusl').write_text('// LUDUS_GLSL_ESX is another name\n')
            _, files = lower_lusl.stage_snapshot(root, 'main.lusl', [], root / 'snapshot')
            self.assertTrue(lower_lusl.references_macro(files, 'LUDUS_GLSL_ES'))
            _, files = lower_lusl.stage_snapshot(root, 'plain.lusl', [], root / 'plain')
            self.assertFalse(lower_lusl.references_macro(files, 'LUDUS_GLSL_ES'))

    def test_glsl_es_lowerings_are_selected_by_the_driver_macro(self):
        module = lower_lusl.combine_variants('float Identity();', 'float Flip();\n', 'LUDUS_GLSL_ES')
        lines = module.splitlines()
        self.assertEqual(lines[1:6], ['#if defined(LUDUS_GLSL_ES)', '#define _LUSL_SELECT_LUDUS_GLSL_ES LUDUS_GLSL_ES',
                                      '#else', '#define _LUSL_SELECT_LUDUS_GLSL_ES 0', '#endif'])
        self.assertEqual(lines[6:], ['#if _LUSL_SELECT_LUDUS_GLSL_ES', 'float Flip();', '#else', 'float Identity();',
                                     '#endif'])

    def test_defines_become_luslc_name_value_pairs(self):
        self.assertEqual(lower_lusl.parse_defines(['DIAGNOSTIC_SCALE=0.5', 'FAST', 'EMPTY=']),
                         ['DIAGNOSTIC_SCALE=0.5', 'FAST=1', 'EMPTY='])

    def test_defines_reject_the_driver_macro_and_malformed_names(self):
        for define in ('LUDUS_GLSL_ES=1', 'LUDUS_GLSL_ES', '2FAST=1', '=1', 'A-B=1'):
            with self.subTest(define=define), contextlib.redirect_stderr(io.StringIO()) as errors:
                with self.assertRaises(SystemExit):
                    lower_lusl.parse_defines([define])
                self.assertIn('DEFINES', errors.getvalue())

    @unittest.skipIf(os.name == 'nt', 'the stand-in luslc is a POSIX script')
    def test_defines_reach_every_lowering(self):
        """The helper's DEFINES reach luslc, beside the driver macro's two lowerings."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'arguments.txt'
            luslc = root / 'luslc'
            luslc.write_text(f'#!{sys.executable}\n'
                             'import sys\n'
                             'arguments = sys.argv[1:]\n'
                             f'open({str(log)!r}, "a").write(" ".join(arguments) + "\\n")\n'
                             'value = lambda key: arguments[arguments.index(key) + 1]\n'
                             'open(value("--output"), "w").write("// lowered\\n")\n'
                             'open(value("--report"), "w").write("binding\\tmain.lusl:1\\tsettings\\tConstantBuffer\\t0\\n")\n')
            luslc.chmod(0o755)
            shaders = root / 'shaders'
            shaders.mkdir()
            (shaders / 'main.lusl').write_text('#if LUDUS_GLSL_ES\n#endif\n')
            output = root / 'out'
            arguments = ['lower_lusl.py', '--luslc', str(luslc), '--root', str(shaders), '--source', 'main.lusl',
                         '--name', 'sample', '--output', str(output / 'sample.lusl.slang'),
                         '--slots', str(output / 'sample_slots.h'), '--depfile', str(output / 'lusl.d'),
                         '--define', 'DIAGNOSTIC_SCALE=0.5', '--define', 'FAST']
            with mock.patch.object(sys, 'argv', arguments):
                self.assertEqual(lower_lusl.main(), 0)
            calls = log.read_text().splitlines()
            self.assertEqual(len(calls), 2)
            for call, glsl_es in zip(calls, ('0', '1')):
                self.assertIn('--define DIAGNOSTIC_SCALE=0.5 --define FAST=1 --define LUDUS_GLSL_ES=' + glsl_es, call)
            self.assertIn('SETTINGS = 0;', (output / 'sample_slots.h').read_text())

    def test_depfile_names_the_requested_target(self):
        """ludus_generate_shader_slots builds only the slot header, so its depfile must name it."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            luslc = root / 'luslc'
            luslc.write_text(f'#!{sys.executable}\n'
                             'import sys\n'
                             'arguments = sys.argv[1:]\n'
                             'value = lambda key: arguments[arguments.index(key) + 1]\n'
                             'open(value("--output"), "w").write("// lowered\\n")\n'
                             'open(value("--report"), "w").write("binding\\tmain.lusl:1\\timage\\tTexture2D\\t0\\n")\n')
            luslc.chmod(0o755)
            shaders = root / 'shaders'
            shaders.mkdir()
            (shaders / 'main.lusl').write_text('Texture2D<float32x4> image;\n')
            output = root / 'out'
            common = ['lower_lusl.py', '--luslc', str(luslc), '--root', str(shaders), '--source', 'main.lusl',
                      '--name', 'sample', '--output', str(output / 'sample.lusl.slang'),
                      '--slots', str(output / 'sample_slots.h'), '--depfile', str(output / 'lusl.d')]
            for extra, target in (([], output / 'sample.lusl.slang'),
                                  (['--depfile-target', str(output / 'sample_slots.h')], output / 'sample_slots.h')):
                with self.subTest(target=target.name), mock.patch.object(sys, 'argv', common + extra):
                    self.assertEqual(lower_lusl.main(), 0)
                    self.assertTrue((output / 'lusl.d').read_text().startswith(lower_lusl.depfile_path(target) + ':'))
                    self.assertIn('IMAGE = 0;', (output / 'sample_slots.h').read_text())


class LuslComputeSlotTests(unittest.TestCase):
    """The benchmark drivers bind by the slots lusl_compute.py reads back from this helper's header."""

    def test_read_slots_returns_bindings_but_not_locations(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import lusl_compute

        header = lower_lusl.slot_header('render', 'binding\tmain.lusl:1\tinputs\tStructuredBuffer\t0\n'
                                                  'binding\tmain.lusl:2\tsceneData\tStructuredBuffer\t1\n'
                                                  'binding\tmain.lusl:3\tparameters\tConstantBuffer\t2\n'
                                                  'vertex-input\tmain.lusl:9\tinput.PositionUv\t5\n')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'render_slots.h'
            path.write_text(header)
            self.assertEqual(lusl_compute.read_slots(path), {'INPUTS': 0, 'SCENE_DATA': 1, 'PARAMETERS': 2})


if __name__ == '__main__':
    unittest.main()
