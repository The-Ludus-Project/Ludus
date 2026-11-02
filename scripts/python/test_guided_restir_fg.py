"""Editable Guided ReSTIR FG+ closure, storage dependencies and installed SDK admission."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from ludus_tools import render_graph as graph, render_graph_build as build
from ludus_tools import render_graph_templates as templates
from ludus_tools import render_graph_ray as ray
from ludus_tools.errors import ToolingError


class GuidedFgTemplateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def create(self, profiles=('metal',)):
        return graph.new_workspace(self.directory / 'Photons', output='offscreen', renderer_static=True,
                                   profiles=profiles, template='guided-restir-fg-plus')

    def test_complete_graph_has_explicit_barriers_and_no_spatial_caustic_reuse(self):
        root = self.create()
        plan = graph.compile_graph(root)
        passes = [value for value in plan['passes'] if value['kind'] == 'RayTracing']
        self.assertEqual(len(passes), 10)
        self.assertEqual([value['id'].rsplit('/', 1)[-1] for value in passes],
                         ['guideDirections', 'guideLights', 'reset', 'photons', 'initial', 'temporal',
                          'spatial', 'caustics', 'record', 'shade'])
        for value in passes:
            self.assertLessEqual(len(value['persistent_buffers']), ray.MAX_BUFFERS)
            self.assertEqual(value['pipeline']['invocation']['traversal'], 'ComputeBvh')
        buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
        self.assertEqual(len(buffers), 5)
        fixed = next(value for value in buffers if "bytes_per_pixel" not in value)
        self.assertEqual(fixed["extent"], [64 * 1024 * 1024, 1])
        self.assertEqual(sorted(value.get('bytes_per_pixel', 0) for value in buffers), [0, 192, 192, 192, 256])
        caustics = next(key for key in plan['resources'] if key.endswith('/causticHistory'))
        self.assertFalse(any(value['resource'] == caustics for value in passes[6]['persistent_buffers']))
        self.assertEqual(passes[7]['persistent_buffers'][1]['resource'], caustics)
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        self.assertEqual(len(manifest['settings']), 10)

    def test_relocated_sdk_has_the_complete_include_closure(self):
        installed = self.directory / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/guided_restir_fg_plus'
        shutil.copytree(source, installed / 'templates/guided_restir_fg_plus')
        with patch.object(templates, '__file__', str(module)):
            files = templates.template_files('guided-restir-fg-plus', ('spirv',), graph)
        for name in ('core', 'storage', 'projection', 'path_evaluation', 'caustic_evaluation', 'transport'):
            self.assertIn('main_shaders/' + name + '.slang', files)
        self.assertEqual(json.loads(files['main.lushader.json'])['profiles'], ['spirv'])

    def test_unsupported_target_and_existing_directory_are_transactional(self):
        with self.assertRaises(ToolingError):
            self.create(('wgsl',))
        self.assertFalse((self.directory / 'Photons').exists())
        (self.directory / 'Photons').mkdir()
        marker = self.directory / 'Photons/mine'
        marker.write_text('preserve')
        with self.assertRaises(ToolingError):
            self.create()
        self.assertEqual(marker.read_text(), 'preserve')

    def test_cli_accepts_the_template(self):
        from ludus_tools.cli import build_parser
        arguments = build_parser().parse_args(['render-graph', 'new', 'Photons', '--template', 'guided-restir-fg-plus'])
        self.assertEqual(arguments.template, 'guided-restir-fg-plus')
