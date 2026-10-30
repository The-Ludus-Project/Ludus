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


class TiledVbufferNeeTemplateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, profiles=('metal', 'wgsl'), name='Tiled'):
        return graph.new_workspace(Path(self.directory.name) / name, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='tiled-vbuffer-nee')

    def test_three_visibility_driven_passes_share_one_pixel_sized_table(self):
        root = self.create()
        plan = graph.compile_graph(root)
        raster = next(operation for operation in plan['passes'] if operation['kind'] == 'Raster')
        rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
        self.assertEqual([operation['id'].rsplit('/', 1)[-1] for operation in rays], ['tiles', 'prefix', 'shade'])
        for operation in rays:
            self.assertEqual(operation['visibility']['resource'], raster['attachments']['color']['resource'])
            self.assertEqual(operation['visibility_depth']['resource'], raster['attachments']['depth']['resource'])
            self.assertEqual(operation['pipeline']['invocation']['dimensions'], 'ColorTarget')
        buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
        self.assertEqual(len(buffers), 1)
        self.assertEqual(buffers[0]['bytes_per_pixel'], 32)
        self.assertEqual(buffers[0]['extent'], [640 * 480 * 32, 1])
        self.assertEqual(buffers[0]['view_scale'], [1, 1])
        final = rays[-1]
        self.assertTrue(any(operation['kind'] == 'Accumulate' for operation in plan['passes']))
        self.assertEqual(1, final['tone_mapping']['exposure'])
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        interface = next(technique['scene_interface'] for technique in manifest['techniques']
                         if 'scene_interface' in technique)
        # One BSDF ray and one light occlusion query per shaded pixel.
        self.assertEqual((interface['visibility_buffer'], interface['surface_queries'], interface['visibility_queries']),
                         (True, 1, 1))
        self.assertFalse(any(program['bindings'] for program in manifest['programs']))

    def test_every_tile_record_fits_the_table_at_common_render_sizes(self):
        import re
        root = self.create()
        header = root.with_name('main_shaders').joinpath('tile_table.slang').read_text()
        constant = lambda name: int(re.search(rf'static const uint {name} = (\d+);', header).group(1))
        tile, words = constant('TILED_NEE_TILE_SIZE_PIXELS'), constant('TILED_NEE_TILE_WORDS')
        library = json.loads(root.with_name('library.lurender.json').read_text())
        table = next(node for node in library['configurations'][0]['nodes'] if node['kind'] == 'Buffer')
        for width, height in ((96, 64), (640, 480), (1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)):
            with self.subTest(width=width, height=height):
                tiles = -(-width // tile) * -(-height // tile)
                self.assertLessEqual(tiles * words * ray.BUFFER_WORD_BYTES, width * height * table['bytes_per_pixel'])
                self.assertLessEqual(width * height * table['bytes_per_pixel'], 256 * 1024 * 1024)

    def test_native_ray_tracing_is_required_and_destinations_are_preserved(self):
        with self.assertRaises(ToolingError):
            self.create(('wgsl',), name='Browser')
        self.assertFalse((Path(self.directory.name) / 'Browser').exists())
        destination = Path(self.directory.name) / 'Tiled'
        destination.mkdir()
        marker = destination / 'mine'
        marker.write_text('keep')
        with self.assertRaises(ToolingError):
            self.create()
        self.assertEqual(list(destination.iterdir()), [marker])

    def test_template_resolves_from_a_relocated_sdk_with_its_include_closure(self):
        from ludus_tools import render_graph_templates
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/tiled_vbuffer_nee'
        shutil.copytree(source, installed / 'templates/tiled_vbuffer_nee')
        with patch.object(render_graph_templates, '__file__', str(module)):
            files = render_graph_templates.template_files('tiled-vbuffer-nee', ('spirv',), graph)
        manifest = json.loads(files['main.lushader.json'])
        self.assertEqual(manifest['profiles'], ['spirv'])
        build.validate_manifest(manifest)
        for callback in ('tiles', 'prefix', 'shade'):
            self.assertIn('#include "tile_table.slang"', files[f'main_shaders/{callback}.slang'])
        self.assertIn('EvaluateTileEmitterPmf', files['main_shaders/tile_table.slang'])

    def test_raster_producer_matches_the_visibility_buffer_template(self):
        from ludus_tools import render_graph_templates as templates
        tiled = templates.template_files('tiled-vbuffer-nee', ('metal',), graph)
        visibility = templates.template_files('visibility-buffer', ('metal',), graph)
        # The editor imports these raster sources from the shared visibility_buffer directory.
        for relative in ('main_shaders/geometry.slang', 'main_shaders/color.slang'):
            self.assertEqual(tiled[relative], visibility[relative])
        self.assertEqual(json.loads(tiled['main.lushader.json'])['schemas'],
                         json.loads(visibility['main.lushader.json'])['schemas'])

    def test_cli_exposes_the_template_choice(self):
        from ludus_tools.cli import build_parser
        arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Main', '--template', 'tiled-vbuffer-nee',
                                               '--output', 'offscreen', '--renderer-static', '--profiles', 'metal'])
        self.assertEqual(arguments.template, 'tiled-vbuffer-nee')


class ComputeRayTracingTemplateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, profiles=('metal', 'wgsl'), name='Compute'):
        return graph.new_workspace(Path(self.directory.name) / name, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='compute-ray-tracing')

    def test_two_scene_compute_passes_share_one_pixel_sized_hit_buffer(self):
        root = self.create()
        plan = graph.compile_graph(root)
        rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
        self.assertEqual([operation['id'].rsplit('/', 1)[-1] for operation in rays], ['primary', 'shade'])
        for operation in rays:
            self.assertEqual(operation['pipeline']['kind'], 'ComputePipeline')
            self.assertEqual(operation['pipeline']['invocation']['dimensions'], 'ColorTarget')
            self.assertNotIn('visibility', operation)
        buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
        self.assertEqual(len(buffers), 1)
        self.assertEqual(buffers[0]['bytes_per_pixel'], 32)
        self.assertEqual(buffers[0]['extent'], [640 * 480 * 32, 1])
        self.assertEqual(buffers[0]['view_scale'], [1, 1])
        # Explicit Accumulate -> ToneMap -> Export, fused into the shade pass.
        self.assertNotIn('accumulation', rays[0])
        self.assertEqual(4096, rays[1]['accumulation']['samples'])
        self.assertEqual(1, rays[1]['tone_mapping']['exposure'])
        self.assertEqual([operation['kind'] for operation in plan['passes'][len(rays):]],
                         ['Accumulate', 'ToneMap', 'Export'])
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        technique = next(technique for technique in manifest['techniques'] if technique['id'] == ray.TECHNIQUE)
        # Node-owned compute passes only: no default callbacks; one primary and one shadow query.
        self.assertFalse(any(stage in technique for stage in ray.STAGES))
        self.assertEqual(technique['scene_interface'], {'version': 1, 'surface_queries': 1, 'visibility_queries': 1})
        compute = [program for program in manifest['programs'] if program['stage'] == 'compute']
        self.assertEqual([(program['id'], program['input'], program['output'], program['bindings'])
                          for program in compute],
                         [('Primary', 'RayLaunch', 'Radiance', []), ('Shade', 'RayLaunch', 'Radiance', [])])

    def test_hit_record_fits_the_buffer_and_keeps_disjoint_random_ranges(self):
        import re
        root = self.create()
        header = root.with_name('main_shaders').joinpath('hit_record.slang').read_text()
        constant = lambda name: int(re.search(rf'static const uint {name} = (\d+);', header).group(1))
        library = json.loads(root.with_name('library.lurender.json').read_text())
        hits = next(node for node in library['configurations'][0]['nodes'] if node['kind'] == 'Buffer')
        self.assertEqual(constant('HIT_RECORD_WORDS') * ray.BUFFER_WORD_BYTES, hits['bytes_per_pixel'])
        self.assertEqual(constant('PRIMARY_RANDOM_DIMENSION_FIRST'), 0)
        self.assertEqual(constant('PRIMARY_RANDOM_DIMENSION_COUNT'), 2)
        self.assertEqual(constant('SHADE_RANDOM_DIMENSION_COUNT'), 4)
        self.assertIn('SHADE_RANDOM_DIMENSION_FIRST = PRIMARY_RANDOM_DIMENSION_FIRST + PRIMARY_RANDOM_DIMENSION_COUNT',
                      header)
        # Zeroed memory must never decode as triangle 0.
        self.assertIn('record.TriangleIndex + 1', header)
        for name in ('Laine', 'Karras', 'Aila', 'HPG 2013', 'Burns', 'Hunt', 'JCGT 2(2), 2013'):
            self.assertIn(name, header)
        for kernel in ('primary', 'shade'):
            source = root.with_name('main_shaders').joinpath(f'{kernel}.slang').read_text()
            self.assertIn('#include "hit_record.slang"', source)
            # Neither pass consumes the automatic stream, which every pass of a sequence shares.
            self.assertNotIn('SampleSceneRandom(queries', source)
            self.assertNotIn('SelectLight(scene, queries', source)
            self.assertNotIn('TraceAndShade', source)

    def test_native_ray_tracing_is_required_and_destinations_are_preserved(self):
        with self.assertRaises(ToolingError):
            self.create(('wgsl',), name='Browser')
        self.assertFalse((Path(self.directory.name) / 'Browser').exists())
        destination = Path(self.directory.name) / 'Compute'
        destination.mkdir()
        marker = destination / 'mine'
        marker.write_text('keep')
        with self.assertRaises(ToolingError):
            self.create()
        self.assertEqual(list(destination.iterdir()), [marker])

    def test_template_resolves_from_a_relocated_sdk_with_its_include_closure(self):
        from ludus_tools import render_graph_templates
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/compute_ray_tracing'
        shutil.copytree(source, installed / 'templates/compute_ray_tracing')
        with patch.object(render_graph_templates, '__file__', str(module)):
            files = render_graph_templates.template_files('compute-ray-tracing', ('spirv',), graph)
        manifest = json.loads(files['main.lushader.json'])
        self.assertEqual(manifest['profiles'], ['spirv'])
        build.validate_manifest(manifest)
        self.assertIn('StoreHitRecord', files['main_shaders/hit_record.slang'])

    def test_raster_fallback_programs_match_the_restir_template(self):
        from ludus_tools import render_graph_templates as templates
        compute = templates.template_files('compute-ray-tracing', ('metal',), graph)
        restir = templates.template_files('restir-di', ('metal',), graph)
        for relative in ('main_shaders/geometry.slang', 'main_shaders/color.slang', 'main_shaders/depth.slang'):
            self.assertEqual(compute[relative], restir[relative])
        self.assertEqual(json.loads(compute['main.lushader.json'])['schemas'],
                         json.loads(restir['main.lushader.json'])['schemas'])

    def test_cli_exposes_the_template_choice(self):
        from ludus_tools.cli import build_parser
        arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Main', '--template', 'compute-ray-tracing',
                                               '--output', 'offscreen', '--renderer-static', '--profiles', 'metal'])
        self.assertEqual(arguments.template, 'compute-ray-tracing')


class VeachGuibasMisTemplateTests(unittest.TestCase):
    """Veach and Guibas multiple importance sampling: MIS and each strategy alone, side by side."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, profiles=('metal', 'wgsl'), name='Mis'):
        return graph.new_workspace(Path(self.directory.name) / name, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='veach-guibas-mis')

    def test_three_shade_passes_share_the_hit_buffer_and_accumulate_separately(self):
        root = self.create()
        plan = graph.compile_graph(root)
        rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
        self.assertEqual([operation['id'].rsplit('/', 1)[-1] for operation in rays],
                         ['primary', 'shade', 'shadeLight', 'shadeBsdf'])
        for operation in rays:
            self.assertEqual(operation['pipeline']['kind'], 'ComputePipeline')
        buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
        self.assertEqual(len(buffers), 1)
        self.assertEqual(buffers[0]['bytes_per_pixel'], 32)
        # Each shade pass owns its target, so Accumulate and ToneMap fuse into it independently.
        self.assertNotIn('accumulation', rays[0])
        for target in ('lightSampling', 'bsdfSampling'):
            self.assertEqual(plan['resources'][f'VeachGuibasMis/transport/{target}']['type'], 'ColorTexture')
        for operation in rays[1:]:
            self.assertEqual(4096, operation['accumulation']['samples'])
            self.assertEqual(1, operation['tone_mapping']['exposure'])
        exports = [operation['id'].rsplit('/', 1)[-1] for operation in plan['passes'] if operation['kind'] == 'Export']
        # MIS is the first export, which the editor viewport shows by default.
        self.assertEqual(exports, ['output', 'lightSamplingOutput', 'bsdfSamplingOutput'])
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        technique = next(technique for technique in manifest['techniques'] if technique['id'] == ray.TECHNIQUE)
        self.assertFalse(any(stage in technique for stage in ray.STAGES))
        # One BSDF ray and one shadow ray per pass fit the shared one-query budgets.
        self.assertEqual(technique['scene_interface'], {'version': 1, 'surface_queries': 1, 'visibility_queries': 1})
        compute = [program for program in manifest['programs'] if program['stage'] == 'compute']
        self.assertEqual([(program['id'], program['function']) for program in compute],
                         [('Primary', 'TracePrimaryHit'), ('ShadeMultipleImportance', 'ShadeMultipleImportance'),
                          ('ShadeLightSampling', 'ShadeLightSampling'), ('ShadeBsdfSampling', 'ShadeBsdfSampling')])

    def test_shade_passes_weight_the_two_strategies_with_mis_settings(self):
        import re
        root = self.create()
        shaders = root.with_name('main_shaders')
        shade = shaders.joinpath('shade.slang').read_text()
        weights = shaders.joinpath('multiple_importance.slang').read_text()
        self.assertIn('#include "multiple_importance.slang"', shade)
        for call in ('SampleIncidentLight', 'SampleStandardBsdf', 'TraceClosest', 'TestVisibility',
                     'EvaluateLightPdfSolidAngle', 'EvaluateEnvironmentPdfSolidAngle', 'ComputeMisWeight'):
            self.assertIn(call, shade)
        # Only the MIS pass weights its strategies; each single-strategy pass keeps weight one.
        self.assertIn('SampleLightStrategy(scene, launch, shadingPoint, true, queries)', shade)
        self.assertIn('SampleBsdfStrategy(scene, launch, shadingPoint, true, queries)', shade)
        self.assertIn('SampleLightStrategy(scene, launch, shadingPoint, false, queries)', shade)
        self.assertIn('SampleBsdfStrategy(scene, launch, shadingPoint, false, queries)', shade)
        for name in ('Veach', 'Guibas', 'SIGGRAPH 1995', '10.1145/218380.218498'):
            self.assertIn(name, weights)
        for heuristic in ('MIS_HEURISTIC_BALANCE = 0', 'MIS_HEURISTIC_POWER = 1', 'MIS_HEURISTIC_CUTOFF = 2',
                          'MIS_HEURISTIC_MAXIMUM = 3'):
            self.assertIn(heuristic, weights)
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        settings = {setting['id']: setting for setting in manifest['settings']}
        self.assertEqual(settings['MIS_HEURISTIC']['value'], 1)
        self.assertEqual((settings['MIS_HEURISTIC']['minimum'], settings['MIS_HEURISTIC']['maximum']), (0, 3))
        self.assertEqual(settings['MIS_POWER_EXPONENT']['value'], 2)
        self.assertEqual(settings['MIS_CUTOFF_THRESHOLD']['value'], 0.1)
        # All three shade passes read one disjoint random range: light selection, shape, BSDF direction.
        header = shaders.joinpath('hit_record.slang').read_text()
        constant = lambda name: int(re.search(rf'static const uint {name} = (\d+);', header).group(1))
        self.assertEqual(constant('PRIMARY_RANDOM_DIMENSION_COUNT'), 2)
        self.assertEqual(constant('SHADE_RANDOM_DIMENSION_COUNT'), 7)
        self.assertIn('SHADE_LIGHT_SELECTION_DIMENSION = SHADE_RANDOM_DIMENSION_FIRST;', header)
        self.assertIn('SHADE_LIGHT_SHAPE_DIMENSION_FIRST = SHADE_LIGHT_SELECTION_DIMENSION + 1;', header)
        self.assertIn('SHADE_BSDF_DIMENSION_FIRST = SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 3;', header)
        for kernel in ('primary', 'shade'):
            source = shaders.joinpath(f'{kernel}.slang').read_text()
            self.assertNotIn('SampleSceneRandom(queries', source)
            self.assertNotIn('SelectLight(scene, queries', source)

    def test_native_ray_tracing_is_required(self):
        with self.assertRaises(ToolingError):
            self.create(('wgsl',), name='Browser')
        self.assertFalse((Path(self.directory.name) / 'Browser').exists())

    def test_relocated_sdk_and_cli_expose_the_template(self):
        from ludus_tools import render_graph_templates
        from ludus_tools.cli import build_parser
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/veach_guibas_mis'
        shutil.copytree(source, installed / 'templates/veach_guibas_mis')
        with patch.object(render_graph_templates, '__file__', str(module)):
            files = render_graph_templates.template_files('veach-guibas-mis', ('spirv',), graph)
        build.validate_manifest(json.loads(files['main.lushader.json']))
        self.assertIn('ComputeMisWeight', files['main_shaders/multiple_importance.slang'])
        arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Mis', '--template', 'veach-guibas-mis',
                                               '--output', 'offscreen', '--renderer-static', '--profiles', 'metal'])
        self.assertEqual(arguments.template, 'veach-guibas-mis')


class LineSamplingTemplateTests(unittest.TestCase):
    """Billen and Dutre's surface-area line estimator in one direction, beside its visible fraction."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, profiles=('metal', 'wgsl'), name='Lines'):
        return graph.new_workspace(Path(self.directory.name) / name, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='line-sampling')

    def test_two_triangle_loop_shade_passes_share_the_hit_buffer_and_accumulate_separately(self):
        for profile in ('metal', 'spirv'):
            with self.subTest(profile=profile):
                root = self.create((profile,), name=profile)
                plan = graph.compile_graph(root)
                rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
                self.assertEqual([operation['id'].rsplit('/', 1)[-1] for operation in rays],
                                 ['primary', 'shade', 'shadeVisibleFraction'])
                # Every pass uses the software triangle loop, so no native ray profile is needed.
                for operation in rays:
                    self.assertEqual(operation['pipeline']['kind'], 'ComputePipeline')
                    self.assertEqual(ray.program_tuple(operation['pipeline'])[-1], ray.TRIANGLE_LOOP_TAG)
                self.assertFalse(ray.requires_native_queries(plan))
                buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
                self.assertEqual([resource['bytes_per_pixel'] for resource in buffers], [32])
                self.assertEqual(plan['resources']['LineSampling/transport/visibleFraction']['type'], 'ColorTexture')
                self.assertNotIn('accumulation', rays[0])
                for operation in rays[1:]:
                    self.assertEqual(4096, operation['accumulation']['samples'])
                    self.assertEqual(1, operation['tone_mapping']['exposure'])
                exports = [operation['id'].rsplit('/', 1)[-1] for operation in plan['passes']
                           if operation['kind'] == 'Export']
                self.assertEqual(exports, ['output', 'visibleFractionOutput'])

    def test_manifest_declares_the_line_budgets_and_kernels(self):
        import re
        root = self.create()
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        technique = next(technique for technique in manifest['techniques'] if technique['id'] == ray.TECHNIQUE)
        self.assertFalse(any(stage in technique for stage in ray.STAGES))
        # The primary hit's surface query, one masked confirmation or non-mesh shadow ray, one chord.
        self.assertEqual(technique['scene_interface'],
                         {'version': 1, 'surface_queries': 1, 'visibility_queries': 1, 'line_queries': 1})
        self.assertNotIn('settings', manifest)
        compute = [program for program in manifest['programs'] if program['stage'] == 'compute']
        self.assertEqual([(program['id'], program['function']) for program in compute],
                         [('Primary', 'TracePrimaryHit'), ('ShadeLineSampling', 'ShadeLineSampling'),
                          ('ShadeVisibleFraction', 'ShadeVisibleFraction')])
        shaders = root.with_name('main_shaders')
        strategy = shaders.joinpath('line_sampling.slang').read_text()
        for call in ('SampleIncidentLight', 'FindVisibleLineIntervals', 'TestVisibility', 'HasMaskedOccluders',
                     'EvaluateEmission'):
            self.assertIn(call, strategy)
        for name in ('Billen', 'Dutre', '10/f84z2h', 'Salesin', 'Jarosz', '10.1111/cgf.13779'):
            self.assertIn(name, strategy)
        # Both shade passes read the line U layout: selection, three shape coordinates, position.
        header = shaders.joinpath('hit_record.slang').read_text()
        constant = lambda name: int(re.search(rf'static const uint {name} = (\d+);', header).group(1))
        self.assertEqual(constant('PRIMARY_RANDOM_DIMENSION_COUNT'), 2)
        self.assertEqual(constant('SHADE_RANDOM_DIMENSION_COUNT'), 5)
        self.assertIn('SHADE_LINE_U_POSITION_DIMENSION = SHADE_LINE_U_DIMENSION_FIRST + 4;', header)
        for kernel in ('primary', 'shade', 'line_sampling'):
            source = shaders.joinpath(f'{kernel}.slang').read_text()
            self.assertNotIn('SampleSceneRandom(queries', source)
            self.assertNotIn('SelectLight(scene, queries', source)

    def test_browser_only_profiles_name_metal_or_vulkan_compute(self):
        with self.assertRaises(ToolingError) as caught:
            self.create(('wgsl',), name='Browser')
        self.assertEqual(caught.exception.code, 'RenderGraphCapability')
        self.assertIn('Metal or Vulkan compute', caught.exception.message)
        self.assertNotIn('native ray tracing', caught.exception.message)
        self.assertFalse((Path(self.directory.name) / 'Browser').exists())

    def test_relocated_sdk_and_cli_expose_the_template(self):
        from ludus_tools import render_graph_templates
        from ludus_tools.cli import build_parser
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/line_sampling'
        shutil.copytree(source, installed / 'templates/line_sampling')
        with patch.object(render_graph_templates, '__file__', str(module)):
            files = render_graph_templates.template_files('line-sampling', ('spirv',), graph)
        build.validate_manifest(json.loads(files['main.lushader.json']))
        self.assertIn('FindVisibleLineIntervals', files['main_shaders/line_sampling.slang'])
        for profile in ('metal', 'spirv'):
            arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Lines', '--template', 'line-sampling',
                                                   '--output', 'offscreen', '--renderer-static', '--profiles', profile])
            self.assertEqual(arguments.template, 'line-sampling')


class RisProjectedSolidAngleTemplateTests(unittest.TestCase):
    """Shah, KT and Narayanan's resampled projected-solid-angle estimator beside its light and BSDF strategies."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, profiles=('metal', 'wgsl'), name='Ris'):
        return graph.new_workspace(Path(self.directory.name) / name, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='ris-projected-solid-angle')

    def test_four_compute_bvh_passes_share_the_hit_buffer_and_accumulate_separately(self):
        for profile in ('metal', 'spirv'):
            with self.subTest(profile=profile):
                root = self.create((profile,), name=profile)
                plan = graph.compile_graph(root)
                rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
                self.assertEqual([operation['id'].rsplit('/', 1)[-1] for operation in rays],
                                 ['primary', 'shade', 'shadeLight', 'shadeBsdf'])
                # Every pass walks the renderer's compute BVH, so no native ray profile is needed.
                for operation in rays:
                    self.assertEqual(operation['pipeline']['kind'], 'ComputePipeline')
                    self.assertEqual(ray.program_tuple(operation['pipeline'])[-1], ray.COMPUTE_BVH_TAG)
                self.assertFalse(ray.requires_native_queries(plan))
                buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
                self.assertEqual([resource['bytes_per_pixel'] for resource in buffers], [32])
                self.assertNotIn('accumulation', rays[0])
                for target in ('lightSampling', 'bsdfSampling'):
                    self.assertEqual(plan['resources'][f'RisProjectedSolidAngle/transport/{target}']['type'],
                                     'ColorTexture')
                for operation in rays[1:]:
                    self.assertEqual(4096, operation['accumulation']['samples'])
                    self.assertEqual(1, operation['tone_mapping']['exposure'])
                exports = [operation['id'].rsplit('/', 1)[-1] for operation in plan['passes']
                           if operation['kind'] == 'Export']
                # The combined estimator is the first export, which the editor viewport shows by default.
                self.assertEqual(exports, ['output', 'lightSamplingOutput', 'bsdfSamplingOutput'])

    def test_manifest_declares_budgets_settings_and_the_polygonal_light_helpers(self):
        root = self.create()
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        technique = next(technique for technique in manifest['techniques'] if technique['id'] == ray.TECHNIQUE)
        self.assertFalse(any(stage in technique for stage in ray.STAGES))
        # One BSDF ray; two shadow rays for the selected triangle's diffuse and LTC samples.
        self.assertEqual(technique['scene_interface'], {'version': 1, 'surface_queries': 1, 'visibility_queries': 2,
                                                        'polygonal_light_sampling': True})
        compute = [program for program in manifest['programs'] if program['stage'] == 'compute']
        self.assertEqual([(program['id'], program['function']) for program in compute],
                         [('Primary', 'TracePrimaryHit'), ('ShadeMultipleImportance', 'ShadeMultipleImportance'),
                          ('ShadeLightSampling', 'ShadeLightSampling'), ('ShadeBsdfSampling', 'ShadeBsdfSampling')])
        values = {setting['id']: (setting['type'], setting['value'], setting['minimum'], setting['maximum'])
                  for setting in manifest['settings']}
        self.assertEqual(values, {'RIS_CANDIDATE_COUNT': ('UInt32', 8, 1, 32),
                                  'RIS_NORMALIZATION_CANDIDATE_COUNT': ('UInt32', 4, 1, 16),
                                  'RIS_TARGET': ('UInt32', 1, 0, 1),
                                  'PROJECTED_SOLID_ANGLE_MIS_VISIBILITY': ('Float32', 0.5, 0, 1)})

    def test_shaders_use_the_shared_primitives_and_credit_their_sources(self):
        root = self.create()
        shaders = root.with_name('main_shaders')
        shade = shaders.joinpath('shade.slang').read_text()
        resampled = shaders.joinpath('resampled_light.slang').read_text()
        light = shaders.joinpath('projected_solid_angle_light.slang').read_text()
        for call in ('SelectLight', 'SampleIncidentLight', 'SampleStandardBsdf', 'TraceClosest', 'TestVisibility',
                     'EvaluateEmission', 'GetSurfaceHit', 'EvaluateEnvironmentPdfSolidAngle',
                     'EvaluateEmissiveTriangleSelectionPmf', 'TrySampleProjectedSolidAnglePolygon',
                     'TrySampleLinearlyTransformedCosinePolygon', 'ComputeClampedOptimalMisWeights',
                     'ComputePowerHeuristicWeight', 'EvaluateSurrogateTriangleProbability'):
            self.assertIn(call, shade)
        for call in ('SelectEmissiveTriangle', 'GetTriangle', 'TryAddReservoirCandidate',
                     'TryComputeReservoirContributionWeight'):
            self.assertIn(call, resampled)
        for call in ('LookupSpecularLtc', 'ComputeSpecularLtcAlbedo', 'TryMakeLinearlyTransformedCosine',
                     'TryPrepareProjectedSolidAnglePolygon', 'TryPrepareLinearlyTransformedCosinePolygon',
                     'TryApproximateClampedCosinePolygonIntegral',
                     'TryApproximateLinearlyTransformedCosinePolygonIntegral'):
            self.assertIn(call, light)
        for name in ('Hill', 'Heitz', 'advances.realtimerendering.com/s2016'):
            self.assertIn(name, light)
        # The helpers come from the wrapper, never from copies inside the package.
        self.assertEqual(sorted(path.name for path in shaders.iterdir()),
                         ['color.slang', 'depth.slang', 'geometry.slang', 'hit_record.slang', 'primary.slang',
                          'projected_solid_angle_light.slang', 'resampled_light.slang', 'shade.slang'])
        for name in ('Shah', 'Narayanan', '10.1145/3610543.3626165'):
            self.assertIn(name, light)
            self.assertIn(name, resampled)
        for name in ('Peters', '10.1145/3450626.3459672'):
            self.assertIn(name, light)
        for name in ('Wyman', 'Ray Tracing Gems II', 'chapter 22'):
            self.assertIn(name, resampled)
        # Only the combined pass weights its strategies; each single-strategy pass keeps weight one.
        self.assertIn('SampleLightStrategy(scene, launch, shadingPoint, true, context, passQueries)', shade)
        self.assertIn('SampleBsdfStrategy(scene, launch, shadingPoint, true, context, passQueries)', shade)
        self.assertIn('SampleLightStrategy(scene, launch, shadingPoint, false, context, passQueries)', shade)
        self.assertIn('SampleBsdfStrategy(scene, launch, shadingPoint, false, context, passQueries)', shade)

    def test_random_dimensions_follow_the_review_table(self):
        import re
        root = self.create()
        shaders = root.with_name('main_shaders')
        header = shaders.joinpath('hit_record.slang').read_text()
        definitions = dict(re.findall(r'static const uint ([A-Z_]+) =\s*([^;]+);', header))

        def evaluate(name):
            expression = definitions[name]
            for other in sorted(definitions, key=len, reverse=True):
                if other in expression:
                    expression = expression.replace(other, str(evaluate(other)))
            return eval(expression, {'__builtins__': {}})

        # Dimensions 2 to 8 keep their veach-guibas-mis meanings.
        self.assertEqual([evaluate(name) for name in ('PRIMARY_RANDOM_DIMENSION_FIRST',
                                                      'PRIMARY_RANDOM_DIMENSION_COUNT',
                                                      'SHADE_LIGHT_SELECTION_DIMENSION',
                                                      'SHADE_LIGHT_SHAPE_DIMENSION_FIRST',
                                                      'SHADE_BSDF_DIMENSION_FIRST')], [0, 2, 2, 3, 6])
        self.assertEqual(evaluate('SHADE_DIFFUSE_TECHNIQUE_DIMENSION_FIRST'), 9)
        self.assertEqual(evaluate('SHADE_SPECULAR_TECHNIQUE_DIMENSION_FIRST'), 12)
        self.assertEqual(evaluate('RIS_CANDIDATE_DIMENSION_FIRST'), 16)
        self.assertEqual(evaluate('RIS_DIMENSIONS_PER_CANDIDATE'), 2)
        self.assertEqual(evaluate('RIS_MAX_CANDIDATE_COUNT'), 32)
        self.assertEqual(evaluate('RIS_NORMALIZATION_DIMENSION_FIRST'), 80)
        self.assertEqual(evaluate('RIS_MAX_NORMALIZATION_CANDIDATE_COUNT'), 16)
        self.assertEqual(evaluate('SHADE_RANDOM_DIMENSION_COUNT'), 94)
        # The table's capacities are the settings' maximums, so a setting never renumbers a dimension.
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        maximums = {setting['id']: setting['maximum'] for setting in manifest['settings']}
        self.assertEqual(maximums['RIS_CANDIDATE_COUNT'], evaluate('RIS_MAX_CANDIDATE_COUNT'))
        self.assertEqual(maximums['RIS_NORMALIZATION_CANDIDATE_COUNT'],
                         evaluate('RIS_MAX_NORMALIZATION_CANDIDATE_COUNT'))
        # Every pass reads explicit addresses, never the automatic stream.
        for kernel in ('primary', 'shade', 'resampled_light'):
            source = shaders.joinpath(f'{kernel}.slang').read_text()
            self.assertNotIn('SampleSceneRandom(queries', source)
            self.assertNotIn('SelectLight(scene, queries', source)

    def test_the_wrapper_includes_the_polygonal_light_helpers_only_when_declared(self):
        snapshot = build.Snapshot(self.directory.name)
        snapshot.files[ray.DEFAULT_PAYLOAD] = (
            Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/shaders/scene_payload.slang').read_bytes()
        selected = {stage: {'source': 'main_shaders/shade.slang', 'function': function, 'output': 'ScenePayload'}
                    for stage, function in zip(ray.STAGES, ('Generate', 'Miss', 'Closest'))}
        interface = {'version': 1, 'surface_queries': 1, 'visibility_queries': 2}
        recipe = {'source': 'renderer_gpu_preview.slang'}
        plain = ray.scene_wrapper(selected, interface, snapshot, build, recipe)
        requested = ray.scene_wrapper(selected, dict(interface, polygonal_light_sampling=True), snapshot, build, recipe)
        includes = ['#include "_ludus_ray/' + source + '"' for source in ray.POLYGONAL_LIGHT_INCLUDES]
        for include in includes:
            self.assertNotIn(include, plain)
            self.assertIn(include, requested)
        # The helpers precede the payload and authored sources, in dependency order.
        positions = [requested.index(include) for include in includes]
        self.assertEqual(positions, sorted(positions))
        self.assertLess(positions[-1], requested.index('#include "main_shaders/shade.slang"'))
        # ltc_table.slang reads the renderer-bound table at the graph's slot, defined before the include.
        binding = '#define LUDUS_LTC_TABLE_BINDING LUDUS_GRAPH_LTC_TABLE_BINDING\n'
        self.assertNotIn(binding, plain)
        self.assertEqual(1, requested.count(binding))
        self.assertLess(requested.index(binding), requested.index('#include "_ludus_ray/ltc_table.slang"'))
        # Every staged helper has one installed owner; the table layout enters only through ltc_table.slang.
        self.assertEqual(set(ray.POLYGONAL_LIGHT_HELPERS) - set(ray.POLYGONAL_LIGHT_INCLUDES), {'ltc_table_layout.h'})
        self.assertFalse(set(ray.POLYGONAL_LIGHT_HELPERS) & set(ray.HELPERS))

    def test_the_polygonal_light_requirement_is_a_boolean(self):
        root = self.create()
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        technique = next(technique for technique in manifest['techniques'] if technique['id'] == ray.TECHNIQUE)
        technique['scene_interface']['polygonal_light_sampling'] = 1
        with self.assertRaises(ToolingError) as caught:
            build.validate_manifest(manifest)
        self.assertIn('polygonal_light_sampling', caught.exception.message)
        # An unknown requirement is rejected rather than ignored, as older SDKs reject this one.
        del technique['scene_interface']['polygonal_light_sampling']
        technique['scene_interface']['polygonal_lights'] = True
        with self.assertRaises(ToolingError):
            build.validate_manifest(manifest)

    def test_browser_only_profiles_name_metal_or_vulkan_compute(self):
        with self.assertRaises(ToolingError) as caught:
            self.create(('wgsl',), name='Browser')
        self.assertEqual(caught.exception.code, 'RenderGraphCapability')
        self.assertIn('Metal or Vulkan compute', caught.exception.message)
        self.assertNotIn('native ray tracing', caught.exception.message)
        self.assertFalse((Path(self.directory.name) / 'Browser').exists())

    def test_relocated_sdk_and_cli_expose_the_template(self):
        from ludus_tools import render_graph_templates
        from ludus_tools.cli import build_parser
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/ris_projected_solid_angle'
        shutil.copytree(source, installed / 'templates/ris_projected_solid_angle')
        with patch.object(render_graph_templates, '__file__', str(module)):
            files = render_graph_templates.template_files('ris-projected-solid-angle', ('spirv',), graph)
        build.validate_manifest(json.loads(files['main.lushader.json']))
        self.assertIn('ComputeClampedOptimalMisWeights', files['main_shaders/projected_solid_angle_light.slang'])
        for profile in ('metal', 'spirv'):
            arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Ris', '--template',
                                                   'ris-projected-solid-angle', '--output', 'offscreen',
                                                   '--renderer-static', '--profiles', profile])
            self.assertEqual(arguments.template, 'ris-projected-solid-angle')


class SalesinJaroszLinesTemplateTests(unittest.TestCase):
    """Salesin and Jarosz's MIS of BSDF samples and line samples in two directions, beside each strategy set."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, profiles=('metal', 'wgsl'), name='PointLine'):
        return graph.new_workspace(Path(self.directory.name) / name, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='salesin-jarosz-lines')

    def test_four_triangle_loop_passes_share_the_hit_buffer_and_accumulate_separately(self):
        for profile in ('metal', 'spirv'):
            with self.subTest(profile=profile):
                root = self.create((profile,), name=profile)
                plan = graph.compile_graph(root)
                rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
                self.assertEqual([operation['id'].rsplit('/', 1)[-1] for operation in rays],
                                 ['primary', 'shade', 'shadeLines', 'shadePoints'])
                # Every pass uses the software triangle loop, so no native ray profile is needed.
                for operation in rays:
                    self.assertEqual(operation['pipeline']['kind'], 'ComputePipeline')
                    self.assertEqual(ray.program_tuple(operation['pipeline'])[-1], ray.TRIANGLE_LOOP_TAG)
                self.assertFalse(ray.requires_native_queries(plan))
                buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
                self.assertEqual([resource['bytes_per_pixel'] for resource in buffers], [32])
                for target in ('lineSampling', 'pointSampling'):
                    self.assertEqual(plan['resources'][f'SalesinJaroszLines/transport/{target}']['type'],
                                     'ColorTexture')
                self.assertNotIn('accumulation', rays[0])
                for operation in rays[1:]:
                    self.assertEqual(4096, operation['accumulation']['samples'])
                    self.assertEqual(1, operation['tone_mapping']['exposure'])
                exports = [operation['id'].rsplit('/', 1)[-1] for operation in plan['passes']
                           if operation['kind'] == 'Export']
                # The combined image is the first export, which the editor viewport shows by default.
                self.assertEqual(exports, ['output', 'lineSamplingOutput', 'pointSamplingOutput'])

    def test_manifest_declares_the_budgets_settings_and_random_layout(self):
        import re
        root = self.create()
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        technique = next(technique for technique in manifest['techniques'] if technique['id'] == ray.TECHNIQUE)
        self.assertFalse(any(stage in technique for stage in ray.STAGES))
        # The maximum over the passes (design D8b): shade needs one BSDF ray, two visibility queries
        # (one per line strategy) and six line queries.
        self.assertEqual(technique['scene_interface'],
                         {'version': 1, 'surface_queries': 1, 'visibility_queries': 2, 'line_queries': 6})
        compute = [program for program in manifest['programs'] if program['stage'] == 'compute']
        self.assertEqual([(program['id'], program['function']) for program in compute],
                         [('Primary', 'TracePrimaryHit'), ('ShadeMultipleImportance', 'ShadeMultipleImportance'),
                          ('ShadeLineSampling', 'ShadeLineSampling'), ('ShadePointSampling', 'ShadePointSampling')])
        values = {setting['id']: setting for setting in manifest['settings']}
        self.assertEqual(sorted(values), ['POINT_LINE_MIS_HEURISTIC', 'POINT_LINE_MIS_POWER_EXPONENT'])
        # Balance is the default, as in Salesin and Jarosz; only balance and power are offered.
        self.assertEqual(values['POINT_LINE_MIS_HEURISTIC']['value'], 0)
        self.assertEqual((values['POINT_LINE_MIS_HEURISTIC']['minimum'], values['POINT_LINE_MIS_HEURISTIC']['maximum']), (0, 1))
        self.assertEqual(values['POINT_LINE_MIS_POWER_EXPONENT']['value'], 2)
        shaders = root.with_name('main_shaders')
        header = shaders.joinpath('hit_record.slang').read_text()
        constant = lambda name: int(re.search(rf'static const uint {name} = (\d+);', header).group(1))
        self.assertEqual(constant('PRIMARY_RANDOM_DIMENSION_COUNT'), 2)
        self.assertEqual(constant('SHADE_RANDOM_DIMENSION_COUNT'), 13)
        # Dimensions 2-6 line U, 7-11 line V, 12-14 BSDF; the area point reuses line U's 2-5.
        for line in ('SHADE_LINE_U_DIMENSION_FIRST = SHADE_RANDOM_DIMENSION_FIRST;',
                     'SHADE_LINE_U_POSITION_DIMENSION = SHADE_LINE_U_DIMENSION_FIRST + 4;',
                     'SHADE_LINE_V_DIMENSION_FIRST = SHADE_LINE_U_POSITION_DIMENSION + 1;',
                     'SHADE_LINE_V_POSITION_DIMENSION = SHADE_LINE_V_DIMENSION_FIRST + 4;',
                     'SHADE_BSDF_DIMENSION_FIRST = SHADE_LINE_V_POSITION_DIMENSION + 1;',
                     'SHADE_POINT_DIMENSION_FIRST = SHADE_LINE_U_DIMENSION_FIRST;'):
            self.assertIn(line, header)
        for kernel in ('primary', 'shade', 'line_sampling', 'multiple_importance'):
            source = shaders.joinpath(f'{kernel}.slang').read_text()
            self.assertNotIn('SampleSceneRandom(queries', source)
            self.assertNotIn('SelectLight(scene, queries', source)

    def test_shade_passes_combine_three_strategies_with_attribution(self):
        root = self.create()
        shaders = root.with_name('main_shaders')
        shade = shaders.joinpath('shade.slang').read_text()
        strategy = shaders.joinpath('line_sampling.slang').read_text()
        weights = shaders.joinpath('multiple_importance.slang').read_text()
        for call in ('SampleStandardBsdf', 'TraceClosest', 'TestVisibility', 'EvaluateLightPdfSolidAngle',
                     'EvaluateEnvironmentPdfSolidAngle', 'EvaluateLineDensity', 'ComputeMisWeight'):
            self.assertIn(call, shade)
        for call in ('SampleIncidentLight', 'FindVisibleLineIntervals', 'HasMaskedOccluders', 'LINE_DIRECTION_U',
                     'LINE_DIRECTION_V', 'EvaluateLineDensity'):
            self.assertIn(call, strategy)
        # MIS weights all three strategies in shade; shadeLines drops BSDF sampling.
        self.assertIn('SampleLineStrategy(scene, launch, shadingPoint, LINE_DIRECTION_U, true, queries)', shade)
        self.assertIn('SampleLineStrategy(scene, launch, shadingPoint, LINE_DIRECTION_V, false, queries)', shade)
        self.assertIn('SampleBsdfStrategy(scene, launch, shadingPoint, true, queries)', shade)
        self.assertIn('SampleBsdfStrategy(scene, launch, shadingPoint, false, queries)', shade)
        self.assertIn('float ComputeMisWeight(float sampledDensity, float otherDensity0, float otherDensity1)', weights)
        for heuristic in ('MIS_HEURISTIC_BALANCE = 0', 'MIS_HEURISTIC_POWER = 1'):
            self.assertIn(heuristic, weights)
        self.assertNotIn('MIS_CUTOFF_THRESHOLD', weights)
        for name in ('Veach', 'Guibas', 'SIGGRAPH 1995', '10.1145/218380.218498', 'Salesin', 'Jarosz',
                     '10.1111/cgf.13779'):
            self.assertIn(name, weights)
        for name in ('Billen', 'Dutre', '10/f84z2h', 'Salesin', 'Jarosz', '10.1111/cgf.13779'):
            self.assertIn(name, strategy)

    def test_heuristic_outside_balance_and_power_is_rejected(self):
        root = self.create()
        original = json.loads(root.with_name('main.lushader.json').read_text())
        index = next(index for index, setting in enumerate(original['settings']) if setting['id'] == 'POINT_LINE_MIS_HEURISTIC')
        for value in (-1, 2, 3, 0.5, True, None):
            with self.subTest(value=value):
                manifest = copy.deepcopy(original)
                manifest['settings'][index]['value'] = value
                with self.assertRaises(ToolingError):
                    build.validate_manifest(manifest)
        for value in (0, 1):
            manifest = copy.deepcopy(original)
            manifest['settings'][index]['value'] = value
            build.validate_manifest(manifest)

    def test_browser_only_profiles_name_metal_or_vulkan_compute(self):
        with self.assertRaises(ToolingError) as caught:
            self.create(('wgsl',), name='Browser')
        self.assertEqual(caught.exception.code, 'RenderGraphCapability')
        self.assertIn('Metal or Vulkan compute', caught.exception.message)
        self.assertFalse((Path(self.directory.name) / 'Browser').exists())

    def test_relocated_sdk_and_cli_expose_the_template(self):
        from ludus_tools import render_graph_templates
        from ludus_tools.cli import build_parser
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/salesin_jarosz_lines'
        shutil.copytree(source, installed / 'templates/salesin_jarosz_lines')
        with patch.object(render_graph_templates, '__file__', str(module)):
            files = render_graph_templates.template_files('salesin-jarosz-lines', ('spirv',), graph)
        build.validate_manifest(json.loads(files['main.lushader.json']))
        self.assertIn('EvaluateLineDensity', files['main_shaders/line_sampling.slang'])
        self.assertIn('ComputeMisWeight', files['main_shaders/multiple_importance.slang'])
        self.assertEqual(render_graph_templates.TEMPLATES['salesin-jarosz-lines'][2], 'compute-ray-tracing')
        for profile in ('metal', 'spirv'):
            arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/PointLine', '--template',
                                                   'salesin-jarosz-lines', '--output', 'offscreen',
                                                   '--renderer-static', '--profiles', profile])
            self.assertEqual(arguments.template, 'salesin-jarosz-lines')

    def test_cli_creates_the_template_for_metal_and_vulkan(self):
        from ludus_tools.cli import main
        for profile in ('metal', 'spirv'):
            with self.subTest(profile=profile):
                destination = Path(self.directory.name) / f'cli-{profile}'
                with patch('sys.stdout'):
                    status = main(['render-graph', 'new', str(destination), '--template', 'salesin-jarosz-lines',
                                   '--output', 'offscreen', '--renderer-static', '--profiles', profile])
                self.assertEqual(status, 0)
                self.assertTrue((destination / 'main_shaders/multiple_importance.slang').is_file())
                plan = graph.compile_graph(destination / 'main.lurender.json')
                self.assertEqual(len([operation for operation in plan['passes']
                                      if operation['kind'] == 'RayTracing']), 4)


class PolygonalLightSamplingTemplateTests(unittest.TestCase):
    """Peters' BRDF importance sampling for polygonal lights: clamped optimal MIS beside its comparison estimators."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def create(self, profiles=('metal', 'wgsl'), name='Polygonal'):
        return graph.new_workspace(Path(self.directory.name) / name, output='offscreen', renderer_static=True,
                                   profiles=profiles, template='polygonal-light-sampling')

    def test_four_compute_bvh_shade_passes_share_the_hit_buffer_and_accumulate_separately(self):
        for profile in ('metal', 'spirv'):
            with self.subTest(profile=profile):
                root = self.create((profile,), name=profile)
                plan = graph.compile_graph(root)
                rays = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
                # The first pass records the primary hits and shades clamped optimal MIS.
                self.assertEqual([operation['id'].rsplit('/', 1)[-1] for operation in rays],
                                 ['shade', 'shadeBalanceHeuristic', 'shadeProjectedSolidAngle', 'shadeLtc'])
                for operation in rays:
                    self.assertEqual(operation['pipeline']['kind'], 'ComputePipeline')
                    self.assertEqual(ray.program_tuple(operation['pipeline'])[-1], ray.COMPUTE_BVH_TAG)
                    self.assertEqual(4096, operation['accumulation']['samples'])
                    self.assertEqual(1, operation['tone_mapping']['exposure'])
                self.assertFalse(ray.requires_native_queries(plan))
                buffers = [resource for resource in plan['resources'].values() if resource['type'] == 'Buffer']
                self.assertEqual([resource['bytes_per_pixel'] for resource in buffers], [32])
                for target in ('balanceHeuristic', 'projectedSolidAngle', 'ltc'):
                    self.assertEqual(plan['resources'][f'PolygonalLightSampling/transport/{target}']['type'],
                                     'ColorTexture')
                exports = [operation['id'].rsplit('/', 1)[-1] for operation in plan['passes']
                           if operation['kind'] == 'Export']
                # Clamped optimal MIS is the first export, which the editor viewport shows by default.
                self.assertEqual(exports, ['output', 'balanceHeuristicOutput', 'projectedSolidAngleOutput',
                                           'ltcOutput'])

    def test_manifest_declares_budgets_the_visibility_setting_and_the_polygonal_light_helpers(self):
        root = self.create()
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        build.validate_manifest(manifest)
        technique = next(technique for technique in manifest['techniques'] if technique['id'] == ray.TECHNIQUE)
        self.assertFalse(any(stage in technique for stage in ray.STAGES))
        # One camera ray; two shadow rays for each of up to eight emissive triangles.
        self.assertEqual(technique['scene_interface'], {'version': 1, 'surface_queries': 1, 'visibility_queries': 16,
                                                        'polygonal_light_sampling': True})
        self.assertEqual(technique['scene_interface']['visibility_queries'],
                         ray.SCENE_QUERY_LIMITS['MAX_VISIBILITY_QUERIES'])
        compute = [program for program in manifest['programs'] if program['stage'] == 'compute']
        self.assertEqual([(program['id'], program['function']) for program in compute],
                         [('ShadeClampedOptimalMis', 'ShadeClampedOptimalMis'),
                          ('ShadeBalanceHeuristic', 'ShadeBalanceHeuristic'),
                          ('ShadeProjectedSolidAngle', 'ShadeProjectedSolidAngle'), ('ShadeLtc', 'ShadeLtc')])
        values = {setting['id']: (setting['type'], setting['value'], setting['minimum'], setting['maximum'])
                  for setting in manifest['settings']}
        self.assertEqual(values, {'PROJECTED_SOLID_ANGLE_MIS_VISIBILITY': ('Float32', 0.5, 0, 1)})

    def test_shaders_use_the_shared_primitives_and_credit_the_paper_by_section(self):
        root = self.create()
        shaders = root.with_name('main_shaders')
        shade = shaders.joinpath('shade.slang').read_text()
        light = shaders.joinpath('polygonal_light.slang').read_text()
        for call in ('GetEmitterCount', 'GetEmitterTriangle', 'GetMaterial', 'GetSurfaceHit', 'EvaluateEmission',
                     'TestVisibility', 'TraceAndStoreHitRecord', 'TryLoadHitRecord'):
            self.assertIn(call, shade)
        for call in ('LookupSpecularLtc', 'ComputeSpecularLtcAlbedo', 'TryMakeLinearlyTransformedCosine',
                     'TryPrepareProjectedSolidAnglePolygon', 'TryPrepareLinearlyTransformedCosinePolygon',
                     'TrySampleProjectedSolidAnglePolygon', 'TrySampleLinearlyTransformedCosinePolygon',
                     'EvaluateProjectedSolidAnglePolygonDensity', 'EvaluateLinearlyTransformedCosinePolygonDensity',
                     'EvaluateStandardBsdf'):
            self.assertIn(call, light)
        # The helpers come from the wrapper, never from copies inside the package.
        self.assertEqual(sorted(path.name for path in shaders.iterdir()),
                         ['color.slang', 'depth.slang', 'geometry.slang', 'hit_record.slang', 'polygonal_light.slang',
                          'shade.slang'])
        for citation in ('Peters', '10.1145/3450626.3459672', '4.1', '4.2', '4.3', 'Departures'):
            self.assertIn(citation, light)
        # One pass per estimator, in output order.
        for estimator in ('CLAMPED_OPTIMAL_MIS_ESTIMATOR', 'BALANCE_HEURISTIC_ESTIMATOR',
                          'PROJECTED_SOLID_ANGLE_ESTIMATOR', 'LTC_ESTIMATOR'):
            self.assertIn(f'ShadeEstimator(scene, launch, {estimator}, queries)', shade)

    def test_random_dimensions_follow_the_documented_table(self):
        import re
        root = self.create()
        shaders = root.with_name('main_shaders')
        header = shaders.joinpath('hit_record.slang').read_text()
        definitions = dict(re.findall(r'static const uint ([A-Z0-9_]+) =\s*([^;]+);', header))

        def evaluate(name):
            expression = definitions[name]
            for other in sorted(definitions, key=len, reverse=True):
                if other in expression:
                    expression = expression.replace(other, str(evaluate(other)))
            return eval(expression, {'__builtins__': {}})

        self.assertEqual([evaluate(name) for name in ('PRIMARY_RANDOM_DIMENSION_FIRST', 'PRIMARY_RANDOM_DIMENSION_COUNT',
                                                      'POLYGONAL_LIGHT_DIMENSION_FIRST',
                                                      'POLYGONAL_LIGHT_DIMENSIONS_PER_LIGHT',
                                                      'PROJECTED_SOLID_ANGLE_SAMPLE_DIMENSION_OFFSET',
                                                      'LTC_SAMPLE_DIMENSION_OFFSET', 'MAX_POLYGONAL_LIGHT_COUNT')],
                         [0, 2, 2, 4, 0, 2, 8])
        # Both MIS passes trace two shadow rays per triangle within the technique's visibility budget.
        manifest = json.loads(root.with_name('main.lushader.json').read_text())
        technique = next(technique for technique in manifest['techniques'] if technique['id'] == ray.TECHNIQUE)
        self.assertEqual(2 * evaluate('MAX_POLYGONAL_LIGHT_COUNT'),
                         technique['scene_interface']['visibility_queries'])
        # Every pass reads explicit addresses, never the automatic stream.
        for kernel in ('shade', 'hit_record', 'polygonal_light'):
            source = shaders.joinpath(f'{kernel}.slang').read_text()
            self.assertNotIn('SampleSceneRandom(queries', source)

    def test_browser_only_profiles_name_metal_or_vulkan_compute(self):
        with self.assertRaises(ToolingError) as caught:
            self.create(('wgsl',), name='Browser')
        self.assertEqual(caught.exception.code, 'RenderGraphCapability')
        self.assertIn('Metal or Vulkan compute', caught.exception.message)
        self.assertFalse((Path(self.directory.name) / 'Browser').exists())

    def test_relocated_sdk_and_cli_expose_the_template(self):
        from ludus_tools import render_graph_templates
        from ludus_tools.cli import build_parser
        installed = Path(self.directory.name) / 'sdk/lib/cmake/Ludus/render_graph'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/polygonal_light_sampling'
        shutil.copytree(source, installed / 'templates/polygonal_light_sampling')
        with patch.object(render_graph_templates, '__file__', str(module)):
            files = render_graph_templates.template_files('polygonal-light-sampling', ('spirv',), graph)
        build.validate_manifest(json.loads(files['main.lushader.json']))
        self.assertIn('ComputePolygonalLightWeight', files['main_shaders/polygonal_light.slang'])
        self.assertEqual(render_graph_templates.TEMPLATES['polygonal-light-sampling'][2], 'compute-ray-tracing')
        for profile in ('metal', 'spirv'):
            arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Polygonal', '--template',
                                                   'polygonal-light-sampling', '--output', 'offscreen',
                                                   '--renderer-static', '--profiles', profile])
            self.assertEqual(arguments.template, 'polygonal-light-sampling')


class SimpleComputeRayTracingTemplateTests(unittest.TestCase):
    def test_single_compute_pass_has_no_hit_buffer_or_accumulation(self):
        with tempfile.TemporaryDirectory() as directory:
            for profile in ('metal', 'spirv'):
                root = graph.new_workspace(Path(directory) / profile, output='offscreen', renderer_static=True,
                                           profiles=(profile, 'wgsl'), template='simple-compute-ray-tracing')
                plan = graph.compile_graph(root)
                self.assertEqual([operation['kind'] for operation in plan['passes']],
                                 ['RayTracing', 'ToneMap', 'Export'])
                operation = plan['passes'][0]
                self.assertEqual(operation['pipeline']['kind'], 'ComputePipeline')
                self.assertNotIn('accumulation', operation)
                self.assertEqual(operation['tone_mapping']['exposure'], 1)
                self.assertFalse(any(resource['type'] == 'Buffer' for resource in plan['resources'].values()))
                manifest = json.loads(root.with_name('main.lushader.json').read_text())
                build.validate_manifest(manifest)
                compute = [program for program in manifest['programs'] if program['stage'] == 'compute']
                self.assertEqual([program['function'] for program in compute], ['TracePixel'])
                source = root.with_name('main_shaders').joinpath('trace.slang').read_text()
                self.assertIn('triangleIndex < triangleCount', source)
                for native_query in ('TraceClosest(', 'TestVisibility(', 'TraceAndShade('):
                    self.assertNotIn(native_query, source)
                build.Snapshot(root.parent).shader_closure('main_shaders/trace.slang')

    def test_relocated_sdk_and_cli_expose_the_same_template(self):
        from ludus_tools import render_graph_templates
        from ludus_tools.cli import build_parser
        with tempfile.TemporaryDirectory() as directory:
            installed = Path(directory) / 'sdk/lib/cmake/Ludus/render_graph'
            module = installed / 'ludus_tools/render_graph_templates.py'
            module.parent.mkdir(parents=True)
            source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/simple_compute_ray_tracing'
            shutil.copytree(source, installed / 'templates/simple_compute_ray_tracing')
            with patch.object(render_graph_templates, '__file__', str(module)):
                files = render_graph_templates.template_files('simple-compute-ray-tracing', ('spirv',), graph)
            self.assertIn('main_shaders/trace.slang', files)
            with self.assertRaises(ToolingError):
                graph.new_workspace(Path(directory) / 'Browser', output='offscreen', renderer_static=True,
                                    profiles=('wgsl',), template='simple-compute-ray-tracing')
            self.assertFalse((Path(directory) / 'Browser').exists())
        arguments = build_parser().parse_args(['render-graph', 'new', 'graphs/Simple', '--template',
                                               'simple-compute-ray-tracing', '--output', 'offscreen',
                                               '--renderer-static', '--profiles', 'metal'])
        self.assertEqual(arguments.template, 'simple-compute-ray-tracing')


class RasterTemplateTests(unittest.TestCase):
    def test_editor_raster_copies_engine_default_and_resizes_both_attachments(self):
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/default'
        for profiles in (('metal', 'wgsl'), ('spirv', 'wgsl'), ('wgsl',)):
            with self.subTest(profiles=profiles), tempfile.TemporaryDirectory() as directory:
                root = graph.new_workspace(Path(directory) / 'Raster', output='offscreen',
                                           renderer_static=True, profiles=profiles)
                expected = {path.relative_to(source).as_posix(): path.read_text()
                            for path in source.rglob('*') if path.is_file()}
                manifest = json.loads(expected.pop('main.lushader.json.in').replace(
                    '@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@', json.dumps(list(profiles))))
                actual = {path.relative_to(root.parent).as_posix(): path.read_text()
                          for path in root.parent.rglob('*') if path.is_file()}
                self.assertEqual(json.loads(actual.pop('main.lushader.json')), manifest)
                self.assertEqual(actual, expected)
                build.validate_manifest(manifest)
                plan = graph.compile_graph(root)
                self.assertEqual(plan['techniques'], ['DefaultStaticOpaque.ColorAndDepth'])
                self.assertEqual([operation['kind'] for operation in plan['passes']], ['Raster', 'Export'])
                for identity in ('color', 'depth'):
                    self.assertEqual(plan['resources']['Main/' + identity]['view_scale'], [1, 1])

                asset = json.loads(root.read_text())
                configuration = asset['configurations'][0]
                output_size = next(node for node in configuration['nodes'] if node['kind'] == 'OutputSize')
                output_size['extent'] = [1921, 1081]
                render_size = next(node for node in configuration['nodes'] if node['kind'] == 'RenderSize')
                render_size['scale'] = [1, 2]
                root.write_bytes(graph.canonical_asset(asset))
                resized = graph.compile_graph(root)
                for identity in ('color', 'depth'):
                    self.assertEqual(resized['resources']['Main/' + identity]['extent'], [961, 541])

    def test_default_clustered_profile_has_an_explicit_optional_prepass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = graph.new_workspace(Path(directory) / 'Raster', output='offscreen',
                                       renderer_static=True, profiles=('wgsl',))
            manifest = json.loads(root.with_name('main.lushader.json').read_text())
            build.validate_manifest(manifest)
            technique = next(item for item in manifest['techniques']
                             if item['id'] == 'DefaultStaticOpaque.ColorAndDepth')
            self.assertEqual(technique['scene_interface'], {'version': 1, 'profile': 'ClusteredDeferred'})
            self.assertEqual(technique['recipes'][0]['depth_compare'], 'LessEqual')
            self.assertEqual(json.loads(root.read_text())['default'], 'Default')
            ordinary = graph.compile_graph(root)
            prepass = graph.compile_graph(root, configuration='ZPrepass')
            self.assertEqual([item['configuration'] for item in ordinary['passes'] if item['kind'] == 'Raster'],
                             ['ColorAndDepth'])
            raster_passes = [item for item in prepass['passes'] if item['kind'] == 'Raster']
            self.assertEqual([item['configuration'] for item in raster_passes], ['DepthOnly', 'ColorAndDepth'])
            self.assertNotIn('color', raster_passes[0]['attachments'])
            written_depth = raster_passes[0]['attachments']['depth']
            loaded_depth = raster_passes[1]['attachments']['depth']
            self.assertEqual(written_depth['resource'], loaded_depth['resource'])
            self.assertEqual(written_depth['store'], 'Store')
            self.assertEqual(loaded_depth['load'], 'Load')
            self.assertEqual(ordinary['resources'], prepass['resources'])
            from ludus_tools import render_graph_execution as execution
            recipes = [{'technique': item['id'], 'state': item['recipes'][0], 'clustered_forward': True}
                       for item in manifest['techniques']]
            lowered = execution.lower_plan({'plans': [prepass], 'recipes': recipes}, 'ZPrepass')[2]
            self.assertEqual([item['AllowsRepeatedCoverage'] for item in lowered], [1, 1])


    def test_relocated_sdk_copies_the_default_without_a_source_checkout(self):
        from ludus_tools import render_graph_templates
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/default'
        with tempfile.TemporaryDirectory() as directory:
            installed = Path(directory) / 'sdk/lib/cmake/Ludus/render_graph'
            module = installed / 'ludus_tools/render_graph_templates.py'
            module.parent.mkdir(parents=True)
            shutil.copytree(source, installed / 'templates/default')
            with patch.object(render_graph_templates, '__file__', str(module)):
                root = graph.new_workspace(Path(directory) / 'Raster', output='offscreen',
                                           renderer_static=True, profiles=('spirv', 'wgsl'))
            self.assertEqual(root.read_bytes(), (source / root.name).read_bytes())
            self.assertEqual(json.loads(root.with_name('main.lushader.json').read_text())['profiles'],
                             ['spirv', 'wgsl'])
            self.assertEqual(graph.compile_graph(root)['roots'][0]['kind'], 'Export')

    def test_missing_default_shader_never_publishes_a_partial_package(self):
        from ludus_tools import render_graph_templates
        source = Path(__file__).resolve().parents[2] / 'modules/graphics/renderer/render_graphs/default'
        with tempfile.TemporaryDirectory() as directory:
            installed = Path(directory) / 'sdk/lib/cmake/Ludus/render_graph'
            module = installed / 'ludus_tools/render_graph_templates.py'
            module.parent.mkdir(parents=True)
            shutil.copytree(source, installed / 'templates/default')
            (installed / 'templates/default/main_shaders/color.slang').unlink()
            destination = Path(directory) / 'Raster'
            with patch.object(render_graph_templates, '__file__', str(module)):
                with self.assertRaises(ToolingError):
                    graph.new_workspace(destination, output='offscreen', renderer_static=True, profiles=('wgsl',))
            self.assertFalse(destination.exists())
            self.assertFalse(list(destination.parent.glob('.Raster.*.staging')))


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


class EditorTemplateCatalogTests(unittest.TestCase):
    """The editor's template catalog offers exactly the templates this tooling can create."""

    def setUp(self):
        catalog = Path(__file__).resolve().parents[2] / 'apps/editor/resources/render_graph_templates.json'
        self.templates = json.loads(catalog.read_text(encoding='utf-8'))['templates']

    def identities(self, kind):
        return [entry['identity'] for entry in self.templates if entry['kind'] == kind]

    def test_graph_entries_match_the_creatable_templates(self):
        from ludus_tools.cli import build_parser
        from ludus_tools.render_graph_templates import TEMPLATES
        graphs = self.identities('graph')
        self.assertEqual(sorted(graphs), sorted(TEMPLATES))
        for identity in graphs:
            arguments = build_parser().parse_args(['render-graph', 'new', 'Main', '--template', identity])
            self.assertEqual(arguments.template, identity)

    def test_shared_graph_settings_have_one_definition(self):
        # Graph settings are package-wide, and Apply Configuration Template rejects a setting whose
        # definition other than its value differs from the package's (MergeTemplateSettings). Any two
        # built-in templates must therefore be importable into one package.
        from ludus_tools import render_graph_templates as templates
        definitions = {}
        for identity in templates.TEMPLATES:
            files = templates.template_files(identity, ('metal',), graph)
            for setting in json.loads(files['main.lushader.json']).get('settings', ()):
                definition = {key: value for key, value in setting.items() if key != 'value'}
                owner, existing = definitions.setdefault(setting['id'], (identity, definition))
                with self.subTest(setting=setting['id'], first=owner, second=identity):
                    self.assertEqual(definition, existing)

    def test_ray_requirements_match_the_tooling_capability_check(self):
        from ludus_tools.render_graph_templates import REQUIREMENT_REPAIRS, TEMPLATES
        for entry in self.templates:
            if entry['kind'] == 'node' or entry['identity'] == 'raster':
                continue
            with self.subTest(kind=entry['kind'], identity=entry['identity']):
                self.assertIn(entry['identity'], TEMPLATES)
                requirement = TEMPLATES[entry['identity']][2]
                ray_requirements = [value for value in entry.get('requirements', ()) if value in REQUIREMENT_REPAIRS]
                self.assertEqual(ray_requirements, [requirement] if requirement else [])


if __name__ == '__main__':
    unittest.main()
