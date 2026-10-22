"""Built-in ReSTIR creation, authored settings and automatic buffer sizing."""

import copy
import json
from pathlib import Path
import tempfile
import shutil
import unittest
from unittest.mock import patch

from ludus_tools import render_graph as graph, render_graph_build as build
from ludus_tools import render_graph_settings as settings
from ludus_tools import render_graph_ray as ray
from ludus_tools.errors import ToolingError


class RestirTemplateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.destination = Path(self.directory.name) / 'Lighting'

    def create(self, profiles=('metal',)):
        return graph.new_workspace(self.destination, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='restir-di')

    def test_complete_editable_package_has_ordered_shared_pixel_buffers(self):
        root = self.create()
        plan = graph.compile_graph(root)
        rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
        self.assertEqual(len(rays), 3)
        buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
        self.assertEqual(len(buffers), 2)
        for resource in buffers:
            self.assertEqual(resource['extent'], [640 * 480 * 64, 1])
            self.assertEqual(resource['view_scale'], [1, 1])
            self.assertEqual(resource['bytes_per_pixel'], 64)
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        self.assertEqual(len(manifest['settings']), 6)
        declarations = settings.shader_source(manifest['settings'])
        self.assertIn('static const uint RESTIR_INITIAL_CANDIDATE_COUNT = 8u;', declarations)
        self.assertIn('static const bool IS_RESTIR_TEMPORAL_ENABLED = true;', declarations)
        before = declarations
        manifest['settings'][0]['value'] = 16
        build.validate_manifest(manifest)
        self.assertNotEqual(before, settings.shader_source(manifest['settings']))
        # Program identities include these generated declarations, including ray wrappers.
        self.assertTrue(build.shader_source({'ray_wrapper': 'ray source', 'settings_source': before}, {}).startswith(before))

    def test_creation_never_replaces_existing_files_or_publishes_browser_only_template(self):
        self.destination.mkdir()
        marker = self.destination / 'mine'
        marker.write_text('keep')
        with self.assertRaises(ToolingError):
            self.create()
        self.assertEqual(marker.read_text(), 'keep')
        self.destination = self.destination.with_name('Browser')
        with self.assertRaises(ToolingError):
            self.create(('wgsl',))
        self.assertFalse(self.destination.exists())

    def test_size_wires_scale_images_and_both_reservoirs_together(self):
        root = self.create()
        asset = json.loads(root.read_text())
        size = next(node for node in asset['configurations'][0]['nodes'] if node['kind'] == 'RenderSize')
        size['extent']['scale'] = [1, 2]
        root.write_bytes(graph.canonical_asset(asset))
        plan = graph.compile_graph(root)
        for resource in plan['resources'].values():
            self.assertEqual(resource['view_scale'], [1, 2])
            self.assertEqual(resource['extent'], [320 * 240 * 64, 1] if resource['type'] == 'Buffer' else [320, 240])
        size['extent'] = [4096, 4096]
        root.write_bytes(graph.canonical_asset(asset))
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(root)
        self.assertIn('reduce the connected RenderSize', str(failure.exception))

    def test_installed_template_is_resolved_without_a_source_checkout(self):
        from ludus_tools import render_graph_templates
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/restir_di'
        shutil.copytree(source, installed / 'templates/restir_di')
        with patch.object(render_graph_templates, '__file__', str(module)):
            files = render_graph_templates.restir_files(('spirv',), graph)
        manifest = json.loads(files['main.lushader.json'])
        self.assertEqual(manifest['profiles'], ['spirv'])
        build.validate_manifest(manifest)

    def test_pixel_stride_policy_matches_the_runtime_format_owner(self):
        import re
        source = Path(__file__).resolve().parents[2] / 'modules/content/render_graph_format/include/ludus/content/render_graph_format/plan.h'
        maximum = int(re.search(r'MAX_RAY_BUFFER_PIXEL_BYTES = (\d+);', source.read_text()).group(1))
        self.assertEqual(ray.MAX_BUFFER_PIXEL_BYTES, maximum)

    def test_invalid_settings_are_rejected_before_shader_compilation(self):
        root = self.create()
        original = json.loads(root.with_name('main.lushader.json').read_text())
        for value in (-1, 0, 33, 1.5, True, float('nan')):
            with self.subTest(value=value):
                manifest = copy.deepcopy(original)
                manifest['settings'][0]['value'] = value
                with self.assertRaises(ToolingError):
                    build.validate_manifest(manifest)
        manifest = copy.deepcopy(original)
        manifest['settings'][0]['id'] = 'Injected;'
        with self.assertRaises(ToolingError):
            build.validate_manifest(manifest)


if __name__ == '__main__':
    unittest.main()
