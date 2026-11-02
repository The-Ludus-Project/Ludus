"""Wave-tracing source/SDK templates and signed spatial shader settings."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from ludus_tools import render_graph as graph
from ludus_tools import render_graph_build as build
from ludus_tools import render_graph_ray as ray
from ludus_tools import render_graph_settings as settings
from ludus_tools import render_graph_templates as templates
from ludus_tools.cli import build_parser
from ludus_tools.errors import ToolingError


class WaveTracingTemplateTests(unittest.TestCase):
    def test_template_exports_wave_geometric_and_aperture_using_compute(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = graph.new_workspace(Path(temporary) / 'Wave', output='offscreen', renderer_static=True,
                                       profiles=('metal',), template='wave-tracing')
            plan = graph.compile_graph(root)
            passes = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
            self.assertEqual([operation['id'].rsplit('/', 1)[-1] for operation in passes],
                             ['primary', 'shadeWave', 'shadeGeometric', 'measureAperture'])
            self.assertFalse(ray.requires_native_queries(plan))
            self.assertTrue(all(operation['pipeline']['invocation']['traversal'] == 'ComputeBvh' for operation in passes))
            manifest = json.loads(root.with_name('main.lushader.json').read_text())
            build.validate_manifest(manifest)
            self.assertEqual(next(t for t in manifest['techniques'] if t['id'] == ray.TECHNIQUE)['scene_interface'],
                             {'version': 1, 'surface_queries': 1, 'visibility_queries': 0})
            self.assertNotIn('tone_mapping', passes[-1])
            self.assertIn('wave.slang', root.parent.joinpath('main_shaders/shade.slang').read_text())

    def test_editor_catalog_snippet_names_the_actual_library_configuration(self):
        root = Path(__file__).resolve().parents[2]
        entries = json.loads((root / 'apps/editor/resources/render_graph_templates.json').read_text())['templates']
        wave = [entry for entry in entries if entry['identity'] == 'wave-tracing']
        self.assertEqual({entry['kind'] for entry in wave}, {'graph', 'snippet'})
        snippet = next(entry for entry in wave if entry['kind'] == 'snippet')
        library = json.loads((root / 'modules/graphics/renderer/render_graphs/wave_tracing/library.lurender.json').read_text())
        self.assertIn(snippet['defaultConfiguration'], [configuration['id'] for configuration in library['configurations']])
        self.assertEqual(snippet['requirements'], ['compute-ray-tracing'])

    def test_relocated_sdk_has_complete_editable_shader_closure(self):
        with tempfile.TemporaryDirectory() as temporary:
            installed = Path(temporary) / 'sdk'
            module = installed / 'ludus_tools/render_graph_templates.py'
            module.parent.mkdir(parents=True)
            source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/wave_tracing'
            shutil.copytree(source, installed / 'templates/wave_tracing')
            with patch.object(templates, '__file__', str(module)):
                files = templates.template_files('wave-tracing', ('spirv',), graph)
            self.assertIn('main_shaders/wave.slang', files)
            build.validate_manifest(json.loads(files['main.lushader.json']))
            arguments = build_parser().parse_args(['render-graph', 'new', 'Wave', '--template', 'wave-tracing',
                                                   '--output', 'offscreen', '--renderer-static', '--profiles', 'metal'])
            self.assertEqual(arguments.template, 'wave-tracing')
            with self.assertRaises(ToolingError):
                templates.template_files('wave-tracing', ('wgsl',), graph)

    def test_spatial_float_settings_allow_negative_coordinates_but_uints_do_not(self):
        signed = dict(id='POSITION_WORLD', label='Position (m)', type='Float32', minimum=-10, value=-2, maximum=10)
        settings.validate([signed], build)
        self.assertIn('-2.0f', settings.shader_source([signed]))
        for change in ({'type': 'UInt32'}, {'value': float('nan')}, {'minimum': -settings.MAX_NUMERIC_VALUE - 1}):
            with self.subTest(change=change), self.assertRaises(ToolingError):
                settings.validate([{**signed, **change}], build)


if __name__ == '__main__':
    unittest.main()
