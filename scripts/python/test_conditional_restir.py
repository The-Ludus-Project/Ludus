"""Conditional ReSTIR's editable source/SDK closure and renderer admission contract."""

import json
from pathlib import Path
import re
import shutil
import tempfile
import unittest
from unittest.mock import patch

from ludus_tools import render_graph as graph
from ludus_tools import render_graph_build as build
from ludus_tools import render_graph_ray as ray
from ludus_tools import render_graph_templates as templates
from ludus_tools.cli import build_parser
from ludus_tools.errors import ToolingError


class ConditionalRestirTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, profiles=('metal',), name='Conditional'):
        return graph.new_workspace(Path(self.directory.name) / name, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='conditional-restir')

    def test_four_passes_and_separate_sample_temporal_spatial_storage(self):
        root = self.create()
        plan = graph.compile_graph(root)
        passes = [p for p in plan['passes'] if p['kind'] == 'RayTracing']
        self.assertEqual([p['id'].rsplit('/', 1)[-1] for p in passes],
                         ['initial', 'temporal', 'spatial', 'shade'])
        buffers = {k.rsplit('/', 1)[-1]: v['bytes_per_pixel'] for k, v in plan['resources'].items()
                   if v['type'] == 'Buffer'}
        reservoir = root.with_name('main_shaders').joinpath('reservoir.slang').read_text()
        words = int(re.search(r'static const uint RESERVOIR_WORDS = (\d+);', reservoir).group(1))
        record_bytes = words * ray.BUFFER_WORD_BYTES
        self.assertEqual(buffers, {name: record_bytes for name in
                                  ('samples', 'temporalReservoirs', 'spatialReservoirs')})
        for p in passes[:3]:
            self.assertNotIn('accumulation', p)
        self.assertGreater(passes[-1]['accumulation']['samples'], 0)
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        settings = {s['id']: s for s in manifest['settings']}
        self.assertEqual(settings['CRIS_PREFIX_BOUNCES']['value'], 1)
        self.assertEqual(settings['CRIS_PREFIX_BOUNCES']['maximum'], 1)
        self.assertEqual(settings['CRIS_INITIAL_CANDIDATES']['minimum'], 1)
        capacity = int(re.search(r'static const uint MAXIMUM_NEIGHBORS = (\d+);', reservoir).group(1))
        self.assertEqual(settings['CRIS_SPATIAL_NEIGHBORS']['maximum'], capacity)
        maximum = int(re.search(r'static const uint MAXIMUM_SUFFIX_BOUNCES = (\d+);', reservoir).group(1))
        self.assertEqual(settings['CRIS_SUFFIX_BOUNCES']['maximum'], maximum)
        self.assertEqual(settings['CRIS_SUFFIX_BOUNCES']['value'], 3)
        technique = next(t for t in manifest['techniques'] if 'scene_interface' in t)
        # Camera + delayed prefix + one query per continuation after the first suffix point.
        self.assertEqual(maximum + settings['CRIS_PREFIX_BOUNCES']['maximum'],
                         technique['scene_interface']['surface_queries'])
        self.assertEqual(technique['scene_interface'], {'version': 1, 'surface_queries': 8, 'visibility_queries': 2})

    def test_direct_and_indirect_settings_preserve_build_and_graph_validity(self):
        root = self.create()
        path = root.with_name('main.lushader.json')
        manifest = json.loads(path.read_text())
        for bounces, suffix in ((0, 0), (1, 0), (0, 3), (1, 3), (1, 7)):
            for s in manifest['settings']:
                if s['id'] == 'CRIS_PREFIX_BOUNCES':
                    s['value'] = bounces
                elif s['id'] == 'CRIS_SUFFIX_BOUNCES':
                    s['value'] = suffix
                elif s['id'] in ('IS_CRIS_TEMPORAL_ENABLED', 'IS_CRIS_SPATIAL_ENABLED'):
                    s['value'] = False
            path.write_text(json.dumps(manifest))
            build.validate_manifest(manifest)
            self.assertTrue(graph.compile_graph(root)['passes'])

    def test_browser_capability_and_existing_destination_are_transactional(self):
        with self.assertRaises(ToolingError):
            self.create(('wgsl',), 'Browser')
        self.assertFalse((Path(self.directory.name) / 'Browser').exists())
        root = self.create(('spirv',))
        before = root.read_bytes()
        with self.assertRaises(ToolingError):
            self.create(('spirv',))
        self.assertEqual(before, root.read_bytes())

    def test_relocated_sdk_and_cli_capture_the_full_shader_closure(self):
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/conditional_restir'
        shutil.copytree(source, installed / 'templates/conditional_restir')
        with patch.object(templates, '__file__', str(module)):
            files = templates.template_files('conditional-restir', ('spirv',), graph)
        build.validate_manifest(json.loads(files['main.lushader.json']))
        for name in ('initial', 'temporal', 'spatial', 'shade', 'reservoir', 'weights', 'tail'):
            self.assertIn(f'main_shaders/{name}.slang', files)
        arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Conditional',
                                               '--template', 'conditional-restir', '--renderer-static',
                                               '--output', 'offscreen', '--profiles', 'metal'])
        self.assertEqual(arguments.template, 'conditional-restir')


if __name__ == '__main__':
    unittest.main()
