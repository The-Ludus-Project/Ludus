"""Capture warm-up metadata follows authored settings and exact legacy template bytes."""
import json
import tempfile
from unittest.mock import patch
from pathlib import Path
import unittest

from ludus_tools import render_graph as graph
from ludus_tools import render_graph_build as build
from ludus_tools import render_graph_settings as settings
from ludus_tools.errors import ToolingError

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / 'modules/graphics/renderer/render_graphs/rao_blackwell_mh'


class CaptureWarmupTests(unittest.TestCase):
    def load_manifest(self):
        return json.loads((TEMPLATE / 'main.lushader.json.in').read_text().replace('@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@', '["metal"]'))

    def test_setting_reference_requires_a_uint32_on_a_scene_compute_program(self):
        manifest = self.load_manifest()
        build.validate_manifest(manifest)
        program = next(item for item in manifest['programs'] if 'capture_warmup_setting' in item)
        for setting_id in ('missing', 'MH_SMALL_STEP_RADIUS'):
            with self.subTest(setting=setting_id):
                program['capture_warmup_setting'] = setting_id
                with self.assertRaises(ToolingError):
                    build.validate_manifest(manifest)

    def test_explicit_setting_uses_edited_value_including_zero(self):
        manifest = self.load_manifest()
        program = next(item for item in manifest['programs'] if 'capture_warmup_setting' in item)
        setting = next(item for item in manifest['settings'] if item['id'] == program['capture_warmup_setting'])
        for count in (0, 7, 128, 1024):
            setting['value'] = count
            self.assertEqual(settings.capture_warmup_samples(program, manifest['settings'], {}), count)

    def test_only_exact_unmodified_legacy_mcmc_source_gets_compatibility_metadata(self):
        manifest = self.load_manifest()
        program = next(item for item in manifest['programs'] if 'capture_warmup_setting' in item)
        del program['capture_warmup_setting']
        source = (TEMPLATE / program['source']).read_bytes()
        sources = {program['source']: source}
        self.assertEqual(settings.capture_warmup_samples(program, manifest['settings'], sources), 128)
        sources[program['source']] = source + b'\n// Custom shader.\n'
        self.assertEqual(settings.capture_warmup_samples(program, manifest['settings'], sources), 0)
        sources[program['source']] = source
        program['function'] = 'CustomEntry'
        self.assertEqual(settings.capture_warmup_samples(program, manifest['settings'], sources), 0)

    def test_cooked_compute_variants_preserve_explicit_and_legacy_warmup(self):
        from test_render_graph_ray import compile_without_tools, stage_helper
        for is_legacy in (False, True):
            with self.subTest(legacy=is_legacy), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                asset = graph.new_workspace(root / 'Transport', output='offscreen', renderer_static=True,
                                            profiles=('metal',), template='rao-blackwell-mh')
                manifest = asset.with_name('main.lushader.json')
                value = json.loads(manifest.read_text())
                for setting in value['settings']:
                    if setting['id'] == 'MH_BURN_IN':
                        setting['value'] = 7
                if is_legacy:
                    for program in value['programs']:
                        program.pop('capture_warmup_setting', None)
                manifest.write_text(json.dumps(value))
                helper = stage_helper(root / 'helper')
                tool = root / 'tool'
                tool.write_text('test tool identity')
                tool.with_name('tool.build.json').write_text('{}')
                with patch.object(build, 'compile_program', compile_without_tools):
                    result = build.build_package(manifest, root / 'build', helper=helper,
                                                 compiler=tool, spirv_cross=tool)
                package = json.loads((Path(result['package']) / 'package.json').read_text())
                recipes = [item for item in package['recipes'] if 'capture_warmup_samples' in item]
                self.assertTrue(recipes)
                for recipe in recipes:
                    self.assertEqual(recipe['capture_warmup_samples'], [7] * len(recipe['ray_programs']))


if __name__ == '__main__':
    unittest.main()
