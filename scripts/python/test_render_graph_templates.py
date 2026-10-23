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
            files = render_graph_templates.template_files('restir-di', ('spirv',), graph)
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


class VisibilityTemplateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, identity, profiles=('metal', 'wgsl')):
        return graph.new_workspace(Path(self.directory.name) / identity, output='offscreen',
                                   renderer_static=True, profiles=profiles, template=identity)

    def test_templates_export_exact_visibility_and_depth_with_shared_view_size(self):
        for identity in ('visibility-buffer', 'visibility-secondary-rays'):
            with self.subTest(identity=identity):
                root = self.create(identity)
                asset = json.loads(root.read_text())
                self.assertEqual(asset['version'], 2)
                nodes = json.loads(root.with_name('library.lurender.json').read_text())['configurations'][0]['nodes']
                self.assertTrue(any(node['kind'] == 'GraphicsPipeline' for node in nodes))
                self.assertFalse(any(node['kind'] == 'Raster' for node in nodes))
                plan = graph.compile_graph(root)
                self.assertTrue(all(resource['view_scale'] == [1, 1] for resource in plan['resources'].values()))
                self.assertTrue(all(resource['samples'] == 1 for resource in plan['resources'].values()))
                raster = next(operation for operation in plan['passes'] if operation['kind'] == 'Raster')
                self.assertEqual(raster['material'], 'DefaultOpaque')
                for aspect, format_name in (('color', 'RGBA8Unorm'), ('depth', 'Depth32Float')):
                    attachment = raster['attachments'][aspect]
                    self.assertEqual((attachment['load'], attachment['store']), ('Clear', 'Store'))
                    self.assertEqual(plan['resources'][attachment['resource']]['format'], format_name)
                    self.assertTrue(any(operation['kind'] == 'Export' and operation['resource'] == attachment['resource']
                                        for operation in plan['passes']))
                manifest = json.loads(root.with_name('main.lushader.json').read_text())
                build.validate_manifest(manifest)
                vertex = next(schema for schema in manifest['schemas'] if schema['id'] == 'RendererStatic')
                self.assertEqual([(field['location'], field['type']) for field in vertex['fields'][-2:]],
                                 [(6, 'Float2'), (7, 'Float2')])
                varying = next(schema for schema in manifest['schemas'] if schema['role'] == 'Varying')
                self.assertTrue(all(field['interpolation'] == 'Flat' for field in varying['fields'][-2:]))
                rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
                self.assertEqual(len(rays), int(identity == 'visibility-secondary-rays'))
                if rays:
                    ray_pass = rays[0]
                    self.assertEqual(ray_pass['visibility']['resource'], raster['attachments']['color']['resource'])
                    self.assertEqual(ray_pass['visibility_depth']['resource'], raster['attachments']['depth']['resource'])
                    self.assertNotEqual(ray_pass['attachments']['color']['resource'], ray_pass['visibility']['resource'])
                    interface = next(technique['scene_interface'] for technique in manifest['techniques']
                                     if 'scene_interface' in technique)
                    self.assertTrue(interface['visibility_buffer'])
                    self.assertEqual(interface['surface_queries'], 1)
                    self.assertEqual(interface['visibility_queries'], 0)
                    self.assertFalse(any(program['bindings'] for program in manifest['programs']))

    def test_shared_render_size_resizes_all_attachments(self):
        root = self.create('visibility-secondary-rays')
        asset = json.loads(root.read_text())
        size = next(node for node in asset['configurations'][0]['nodes'] if node['kind'] == 'RenderSize')
        size['extent'] = [96, 64]
        root.write_bytes(graph.canonical_asset(asset))
        plan = graph.compile_graph(root)
        self.assertTrue(all(resource['extent'] == [96, 64] for resource in plan['resources'].values()))
        ray_pass = next(operation for operation in plan['passes'] if operation['kind'] == 'RayTracing')
        self.assertEqual(ray_pass['pipeline']['invocation']['dimensions'], 'ColorTarget')

    def test_raster_is_portable_but_secondary_rays_require_a_native_target(self):
        self.create('visibility-buffer', ('wgsl',))
        with self.assertRaises(ToolingError):
            self.create('visibility-secondary-rays', ('wgsl',))
        self.assertFalse((Path(self.directory.name) / 'visibility-secondary-rays').exists())

    def test_existing_destinations_are_preserved(self):
        for identity in ('visibility-buffer', 'visibility-secondary-rays'):
            destination = Path(self.directory.name) / identity
            destination.mkdir()
            marker = destination / 'mine'
            marker.write_text('keep')
            with self.assertRaises(ToolingError):
                self.create(identity)
            self.assertEqual(list(destination.iterdir()), [marker])
            self.assertEqual(marker.read_text(), 'keep')

    def test_both_templates_resolve_from_a_relocated_sdk(self):
        from ludus_tools import render_graph_templates
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        for identity, directory in (('visibility-buffer', 'visibility_buffer'),
                                     ('visibility-secondary-rays', 'visibility_secondary_rays')):
            source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs' / directory
            shutil.copytree(source, installed / 'templates' / directory)
            with patch.object(render_graph_templates, '__file__', str(module)):
                files = render_graph_templates.template_files(identity, ('spirv', 'wgsl'), graph)
            manifest = json.loads(files['main.lushader.json'])
            self.assertEqual(manifest['profiles'], ['spirv', 'wgsl'])
            build.validate_manifest(manifest)
            self.assertIn('EncodeVisibility', files['main_shaders/color.slang'])

    def test_cli_exposes_both_template_choices(self):
        from ludus_tools.cli import build_parser
        for identity in ('visibility-buffer', 'visibility-secondary-rays'):
            arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Main', '--template', identity,
                                                   '--output', 'offscreen', '--renderer-static', '--profiles', 'metal'])
            self.assertEqual(arguments.template, identity)


class ForwardLitTemplateTests(unittest.TestCase):
    def test_forward_template_is_portable_and_declares_one_scene_material_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            root = graph.new_workspace(Path(directory) / 'Lighting', output='offscreen',
                                       renderer_static=True, profiles=('wgsl',), template='forward-lit')
            plan = graph.compile_graph(root)
            manifest = json.loads(root.with_name('main.lushader.json').read_text())
            build.validate_manifest(manifest)
            self.assertEqual(manifest['profiles'], ['wgsl'])
            self.assertEqual(manifest['techniques'][0]['scene_interface'], {'version': 1, 'profile': 'ForwardLit'})
            self.assertEqual(plan['techniques'], ['DefaultStaticOpaque.ColorAndDepth'])
            library = json.loads(root.with_name('rasterize.lurender.json').read_text())
            self.assertEqual(library['configurations'][0]['nodes'][0]['kind'], 'GraphicsPipeline')
            extra_recipe = dict(manifest['techniques'][0]['recipes'][0], id='Extra')
            manifest['techniques'][0]['recipes'].append(extra_recipe)
            with self.assertRaises(ToolingError):
                build.validate_manifest(manifest)
            manifest['techniques'][0]['recipes'].pop()
            manifest['techniques'][0]['scene_interface']['version'] = 2
            with self.assertRaises(ToolingError):
                build.validate_manifest(manifest)


if __name__ == '__main__':
    unittest.main()
