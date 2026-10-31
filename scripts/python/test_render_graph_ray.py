"""Production validation for authored triangle ray graph contracts."""
from copy import deepcopy
import json
import hashlib
import os
import shutil
import struct
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from ludus_tools import render_graph as graph, render_graph_build as build, render_graph_ray as ray
from ludus_tools.errors import ToolingError
from ludus_tools import render_graph_pipeline as pipeline
from ludus_tools import render_graph_execution as execution
from ludus_tools import render_graph_ray_invocation as invocation
from ludus_tools import render_graph_reload as reload

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'tests/sdk_consumer/rg2/raytracing'
# The source-tree owners of the flat SDK shaders directory, in _ludus_stage_render_graph_helpers order.
HELPER_ROOTS = ('cmake/shaders', 'modules/graphics/renderer/shaders',
                'modules/content/scene_format/include/ludus/content/scene_format',
                'modules/content/render_graph_format/include/ludus/content/render_graph_format',
                'modules/foundation/math/shaders', 'modules/graphics/rhi/shaders',
                'modules/graphics/renderer/include/ludus/graphics/renderer',
                'modules/graphics/renderer/include/ludus/graphics/renderer/detail', 'config')


def stage_helper(destination):
    """Stage the installed helper layout without CMake; each name keeps its first owner."""
    destination.mkdir()
    for owner in HELPER_ROOTS:
        for path in sorted((ROOT / owner).iterdir()):
            if path.is_file() and not (destination / path.name).exists():
                shutil.copy(path, destination / path.name)
    return destination


def compile_without_tools(program, schemas, closure, snapshot, work, helper, tools, target, cancel, *_):
    """Stand in for the pinned compiler with a stage result that has no bindings."""
    output = work / 'artifact'
    output.mkdir()
    (output / 'artifact.txt').write_text(program['id'] + ' ' + target + '\n')
    result = {'version': 1, 'target': target, 'stage': program['stage'], 'source_entry': 'rgEntry', 'entry': 'rgEntry',
              'artifact': 'artifact.txt', 'interface': {'entries': [], 'inputs': []}, 'links': [], 'layouts': {},
              'sha256': build.digest((output / 'artifact.txt').read_bytes())}
    return output, result


def make_legacy_scene_bridge(root):
    """Reproduce the old editor bridge after replacing its closest-hit source."""
    helper = root / 'main_shaders/scene_legacy_callbacks.slang'
    declaration = (ROOT / 'modules/graphics/renderer/shaders/ray_shader.slang').read_text().split('struct RayHit', 1)[1]
    helper.write_text('#pragma once\n// Compatibility inputs for the original authored callbacks.\n'
                      'struct RayHit' + declaration +
                      '#include "closest.slang"\n#include "legacy_miss.slang"\n#include "legacy_generate.slang"\n')
    (helper.parent / 'legacy_miss.slang').write_text('float3 OriginalMiss(Ray ray) { return float3(0); }\n')
    (helper.parent / 'legacy_generate.slang').write_text('Ray OriginalGenerate(RayLaunch launch) { return launch.CameraRay; }\n')
    (helper.parent / 'miss.slang').write_text('''#include "scene_legacy_callbacks.slang"
void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    payload.Radiance = OriginalMiss(ray);
}
''')
    (helper.parent / 'generate.slang').write_text('''#include "scene_legacy_callbacks.slang"
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, queries, OriginalGenerate(launch), payload);
    return payload.Radiance;
}
''')
    (helper.parent / 'closest.slang').write_text('''#include "scene_legacy_callbacks.slang"
#define LUDUS_EMITTER_SAMPLING_MODE_UNIFORM (0)
struct ReplacementLighting { float3 Radiance; };
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    ReplacementLighting lighting;
    lighting.Radiance = EvaluateMaterial(scene, hit).BaseColor;
    payload.Radiance = lighting.Radiance;
}
''')
    return helper


class RayHelperStagingTests(unittest.TestCase):
    def test_staged_helpers_carry_every_helper_file_they_include(self):
        with tempfile.TemporaryDirectory() as temporary:
            helper = Path(temporary) / 'helper'
            helper.mkdir()
            # An engine include added later, as material_scattering.slang was, needs no HELPERS entry.
            (helper / 'root.slang').write_text('#include "added.slang"\n#if 0\n#    include "branch.h"\n#endif\n'
                                               '#include <ludus/system.h>\n#include "missing.slang"\n')
            (helper / 'added.slang').write_text('#include "root.slang"\n#include "nested.h"\n')
            (helper / 'nested.h').write_text('// leaf\n')
            (helper / 'branch.h').write_text('// leaf\n')
            (helper / 'unused.slang').write_text('// never included\n')
            snapshot = build.Snapshot(Path(temporary) / 'sources')
            ray.stage_helpers(snapshot, helper, ('root.slang',), build)
            self.assertEqual(sorted(snapshot.files), ['_ludus_ray/added.slang', '_ludus_ray/branch.h',
                                                      '_ludus_ray/nested.h', '_ludus_ray/root.slang'])
            self.assertEqual(snapshot.external['_ludus_ray/nested.h'], helper / 'nested.h')

    @unittest.skipUnless(os.environ.get('LUDUS_RG1_HELPER'), 'requires a staged SDK shader helper')
    def test_engine_ray_helpers_include_only_staged_or_external_files(self):
        helper = Path(os.environ['LUDUS_RG1_HELPER'])
        recipes = json.loads((helper / 'recipes.json').read_text())
        roots = (*ray.HELPERS, *ray.POLYGONAL_LIGHT_HELPERS, recipes['renderer_gpu_preview']['source'],
                 recipes['renderer_gpu_preview_image']['source'])
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = build.Snapshot(Path(temporary))
            ray.stage_helpers(snapshot, helper, roots, build)
            for relative, data in snapshot.files.items():
                for name in ray.HELPER_INCLUDE.findall(data.decode('utf-8', 'replace')):
                    if (helper / name).is_file():
                        self.assertIn('_ludus_ray/' + name, snapshot.files, relative)


class RayPipelineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'main.lurender.json'
        self.document = json.loads((FIXTURE / self.path.name).read_text())
        self.node = next(value for value in self.document['configurations'][0]['nodes'] if value['kind'] == 'RayTracingPipeline')

    def compile(self):
        self.path.write_bytes(graph.canonical_asset(self.document))
        return graph.compile_graph(self.path)

    def test_legacy_scene_bridge_migration_preserves_snapshot_and_diagnostic_lines(self):
        root = self.path.parent
        shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray/main_shaders', root / 'main_shaders')
        helper = make_legacy_scene_bridge(root)
        # Comments are not dependencies; CRLF projects use the same migration.
        helper.write_bytes((helper.read_text() + '/*\n#include "missing.slang"\n*/\n').replace('\n', '\r\n').encode())
        snapshot = build.Snapshot(root)
        original = helper.read_bytes()
        compatibility = ray.migrate_legacy_scene_sources(['main_shaders/closest.slang'], snapshot)
        shared = compatibility['sources']['main_shaders/scene_legacy_callbacks.slang']
        self.assertNotIn('struct RayHit', shared)
        self.assertNotIn('#include "closest.slang"', shared)
        self.assertEqual(original.count(b'\n'), shared.count('\n'))
        self.assertEqual(['main_shaders/closest.slang', 'main_shaders/legacy_generate.slang',
                          'main_shaders/legacy_miss.slang'], compatibility['callbacks'])
        self.assertEqual(original, snapshot.files['main_shaders/scene_legacy_callbacks.slang'])
        snapshot.verify()

    def test_legacy_scene_bridge_migration_leaves_custom_declarations_and_directives_alone(self):
        root = self.path.parent
        shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray/main_shaders', root / 'main_shaders')
        helper = make_legacy_scene_bridge(root)
        original = helper.read_text()
        for addition in ('float3 CustomLighting(float3 color) { return color; }\n',
                         '#define CUSTOM_LIGHTING 1\n', '#if CUSTOM_LIGHTING\n#endif\n'):
            with self.subTest(addition=addition):
                helper.write_text(original + addition)
                snapshot = build.Snapshot(root)
                self.assertEqual({'sources': {}, 'callbacks': []},
                                 ray.migrate_legacy_scene_sources(['main_shaders/closest.slang'], snapshot))
                snapshot.verify()

    def test_ray_image_dependencies_exclude_callbacks_and_preserve_engine_helpers(self):
        root = self.path.parent
        (root / 'closest.slang').write_text('#include "shared.slang"\n// callback\n')
        (root / 'shared.slang').write_text('// authored dependency\n')
        snapshot = build.Snapshot(root)
        engine = '_ludus_ray/image.slang'
        snapshot.files[engine] = b'// image recipe\n'
        snapshot.external[engine] = root / 'installed-image.slang'
        image = {'id': ray.IMAGE_VERTEX_ID, 'ray_wrapper': '#include "' + engine + '"\n'}
        compute = {'id': ray.COMPUTE_ID, 'ray_wrapper': image['ray_wrapper'] + '#include "closest.slang"\n'}
        self.assertEqual([engine], ray.capture_program_closure(image, snapshot, build))
        self.assertEqual(sorted([engine, 'closest.slang', 'shared.slang']),
                         ray.capture_program_closure(compute, snapshot, build))
        snapshot.load('closest.slang')
        self.assertEqual([engine], ray.capture_program_closure(image, snapshot, build))

    def test_visibility_and_ordinary_configurations_keep_separate_callback_profiles(self):
        visibility = graph.compile_graph(ROOT / 'tests/sdk_consumer/rg2/visibility/main.lurender.json')
        ordinary = self.compile()
        ordinary['configuration'] = 'Ordinary'
        technique = {'scene_interface': {'version': 1, 'visibility_buffer': True}}
        profiles = ray.group_profiles([ordinary, visibility], technique, build)
        self.assertEqual([False, True], [profile['visibility_buffer'] for profile in profiles])
        self.assertEqual([ray.COMPUTE_ID, ray.VISIBILITY_COMPUTE_ID], [profile['compute'] for profile in profiles])
        self.assertTrue(all(not profile['persistent'] for profile in profiles))
        self.assertEqual([ray.program_tuple(ordinary['passes'][0]['pipeline'])], profiles[0]['variants'])
        with self.assertRaisesRegex(ToolingError, 'visibility_buffer'):
            ray.group_profiles([visibility], {'scene_interface': {}}, build)
        mixed = deepcopy(visibility)
        mixed['passes'].extend(ordinary['passes'])
        with self.assertRaisesRegex(ToolingError, 'separate configurations'):
            ray.group_profiles([mixed], technique, build)

    def test_visibility_fixture_retains_raster_and_reads_matching_versions(self):
        source = ROOT / 'tests/sdk_consumer/rg2/visibility/main.lurender.json'
        plan = graph.compile_graph(source)
        operations = [p for p in plan['passes'] if p['kind'] in ('Raster', 'RayTracing')]
        self.assertEqual(['Raster', 'RayTracing'], [p['kind'] for p in operations])
        producer, consumer = operations
        for key, aspect in (('visibility', 'color'), ('visibility_depth', 'depth')):
            self.assertEqual(producer['attachments'][aspect]['resource'], consumer[key]['resource'])
            self.assertEqual(producer['attachments'][aspect]['output_version'], consumer[key]['input_version'])
        manifest = json.loads((source.parent / 'main.lushader.json.in').read_text()
                              .replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
        build.validate_manifest(manifest)

    def test_visibility_sequence_shares_one_raster_image_across_ray_passes(self):
        plan = graph.compile_graph(ROOT / 'tests/sdk_consumer/rg2/visibility_sequence/main.lurender.json')
        operations = [p for p in plan['passes'] if p['kind'] in ('Raster', 'RayTracing')]
        self.assertEqual(['Raster', 'RayTracing', 'RayTracing', 'RayTracing'], [p['kind'] for p in operations])
        producer = operations[0]
        for consumer in operations[1:]:
            for key, aspect in (('visibility', 'color'), ('visibility_depth', 'depth')):
                self.assertEqual(producer['attachments'][aspect]['resource'], consumer[key]['resource'])
                self.assertEqual(producer['attachments'][aspect]['output_version'], consumer[key]['input_version'])
        technique = {'scene_interface': {'version': 1, 'visibility_buffer': True}}
        profiles = ray.group_profiles([plan], technique, build)
        self.assertEqual([True], [profile['visibility_buffer'] for profile in profiles])
        self.assertTrue(profiles[0]['persistent'])
        self.assertEqual(3, len(profiles[0]['variants']))
        state = {'color': 'RGBA8Unorm', 'depth': 'Depth32Float', 'samples': 1, 'topology': 'TriangleList', 'cull': 'None',
                 'front': 'CounterClockwise', 'depth_compare': 'Less', 'depth_write': True, 'blend': 'Opaque'}
        package = {'plans': [plan], 'recipes': [{'technique': producer['technique'], 'state': state}]}
        rays = [p for p in execution.lower_plan(package, 'Default')[2] if p['Program'] == 'RayTracing']
        self.assertEqual(3, len(rays))
        self.assertEqual(1, len({(p['Visibility'], p['VisibilityDepth']) for p in rays}))
        self.assertNotEqual(execution.ABSENT, rays[0]['Visibility'])
        stale = deepcopy(plan)
        [p for p in stale['passes'] if p['kind'] == 'RayTracing'][2]['visibility']['input_version'] += 1
        with self.assertRaisesRegex(ToolingError, 'same visibility/depth versions'):
            execution.lower_plan(dict(package, plans=[stale]), 'Default')
        mixed = deepcopy(plan)
        middle = [p for p in mixed['passes'] if p['kind'] == 'RayTracing'][1]
        del middle['visibility'], middle['visibility_depth']
        with self.assertRaisesRegex(ToolingError, 'separate configurations'):
            ray.group_profiles([mixed], technique, build)

    def test_visibility_shading_dispatches_each_pass_callbacks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'payload.slang').write_text('struct ScenePayload { float3 Radiance; };\n')
            programs = {}
            for name, stage, function in (('Generate', 'ray_generation', 'GenerateRay'), ('Miss', 'miss', 'ShadeMiss'),
                                          ('First', 'closest_hit', 'ShadeFirst'), ('Second', 'closest_hit', 'ShadeSecond')):
                (root / (name + '.slang')).write_text('// ' + function + '\n')
                programs[name] = {'id': name, 'stage': stage, 'source': name + '.slang', 'function': function,
                                  'output': 'Radiance' if stage == 'ray_generation' else 'ScenePayload'}
            interface = {'version': 1, 'payload': 'payload.slang', 'surface_queries': 1, 'visibility_queries': 0}
            variants = [('Generate', 'Miss', 'First'), ('Generate', 'Miss', 'Second')]
            recipe = {'source': 'renderer_gpu_preview.slang'}
            for has_visibility in (False, True):
                technique = {'scene_interface': dict(interface, visibility_buffer=has_visibility)}
                wrapper = ray.persistent_wrapper(programs, technique, variants, build.Snapshot(root), build, recipe)
                self.assertEqual(has_visibility, 'uint LoadVisibilityAndShade(' in wrapper)
                if not has_visibility:
                    continue
                # Declared ahead of the namespaced sources that call it.
                self.assertLess(wrapper.index('uint LoadVisibilityAndShade('), wrapper.index('namespace GraphSource'))
                body = wrapper.split('uint LoadVisibilityAndShade(SceneContext scene, inout RayQueryContext queries, '
                                     'RayLaunch launch, inout ScenePayload payload)\n{', 1)[1].split('\n}\n', 1)[0]
                self.assertIn('LoadVisibilitySurface(scene, launch, queries, hit)', body)
                self.assertEqual(2, body.count('switch (GetGraphPassIndex())'))
                self.assertIn('ShadeFirst(scene, hit, queries, payload)', body)
                self.assertIn('ShadeSecond(scene, hit, queries, payload)', body)
                self.assertIn('ShadeMiss(scene, ray, queries, payload)', body)

    def test_authored_stages_lower_to_ray_execution_and_camera_export(self):
        plan = self.compile()
        operation = next(value for value in plan['passes'] if value['kind'] == 'RayTracing')
        self.assertEqual(ray.TECHNIQUE, operation['technique'])
        self.assertEqual({'color'}, set(operation['attachments']))
        self.assertEqual('Main/scene', operation['scene'])
        self.assertEqual('Main/camera', operation['view'])
        self.assertEqual('Main/color', plan['roots'][0]['resource'])
        self.assertEqual(self.node, operation['pipeline'])
        self.assertEqual(self.document, json.loads(self.path.read_text()))

    def test_compute_pipeline_reuses_ray_programs_and_defaults_to_triangle_intersections(self):
        original = deepcopy(self.node['shaders'])
        self.node.update(kind='ComputePipeline', state={})
        plan = self.compile()
        operation = next(item for item in plan['passes'] if item['kind'] == 'RayTracing')
        self.assertEqual(original, operation['pipeline']['shaders'])
        variant = ray.program_tuple(operation['pipeline'])
        self.assertEqual(('Generate', 'Miss', 'Closest'), ray.callback_programs(variant))
        self.assertEqual(ray.TRIANGLE_LOOP_TAG, variant[-1])
        self.assertFalse(ray.is_compute_variant(variant))
        self.node['invocation']['traversal'] = 'NativeQuery'
        self.assertEqual(('Generate', 'Miss', 'Closest'), ray.program_tuple(self.compile()['passes'][0]['pipeline']))

    def test_shared_callbacks_reject_partial_or_mixed_kernel_assignments(self):
        self.node.update(kind='ComputePipeline', state={})
        original = deepcopy(self.node['shaders'])
        for shaders in (original[:-1], original + [{'id': 'kernel', 'stage': 'compute', 'program': 'Kernel'}]):
            self.node['shaders'] = shaders
            with self.assertRaisesRegex(ToolingError, 'without a compute kernel'):
                self.compile()

    def test_traversal_selection_is_validated_and_cooked_as_part_of_the_variant(self):
        original = ray.program_tuple(self.node)
        self.node['invocation']['traversal'] = 'TriangleLoop'
        software_plan = self.compile()
        software = ray.program_tuple(software_plan['passes'][0]['pipeline'])
        self.assertEqual(original + (ray.TRIANGLE_LOOP_TAG,), software)
        self.assertEqual(original, ray.callback_programs(software))
        native_plan = deepcopy(software_plan)
        native_plan['passes'][0]['pipeline']['invocation']['traversal'] = 'NativeQuery'
        groups = ray.group_profiles([software_plan, native_plan], {}, build)
        self.assertEqual([software], groups[0]['variants'])
        self.assertFalse(groups[0]['native_ray_queries'])
        self.assertEqual([original], groups[1]['variants'])
        self.assertTrue(groups[1]['native_ray_queries'])
        self.assertTrue(groups[0]['persistent'])
        for strategy in ('Unknown', '', 1, None, 'computebvh'):
            self.node['invocation']['traversal'] = strategy
            with self.assertRaisesRegex(ToolingError, 'traversal'):
                self.compile()

    def test_compute_bvh_is_a_software_strategy_with_its_own_variant(self):
        original = ray.program_tuple(self.node)
        self.node['invocation']['traversal'] = 'ComputeBvh'
        bvh_plan = self.compile()
        bvh = ray.program_tuple(bvh_plan['passes'][0]['pipeline'])
        self.assertEqual(original + (ray.COMPUTE_BVH_TAG,), bvh)
        self.assertEqual(original, ray.callback_programs(bvh))
        self.assertEqual('ComputeBvh', ray.get_traversal(bvh))
        self.assertFalse(ray.requires_native_queries(bvh_plan))
        loop_plan = deepcopy(bvh_plan)
        loop_plan['passes'][0]['pipeline']['invocation']['traversal'] = 'TriangleLoop'
        loop = ray.program_tuple(loop_plan['passes'][0]['pipeline'])
        self.assertEqual('TriangleLoop', ray.get_traversal(loop))
        native_plan = deepcopy(bvh_plan)
        native_plan['passes'][0]['pipeline']['invocation']['traversal'] = 'NativeQuery'
        self.assertEqual('NativeQuery', ray.get_traversal(ray.program_tuple(native_plan['passes'][0]['pipeline'])))
        # Both software strategies share one persistent software group with distinct cooked variants.
        groups = ray.group_profiles([bvh_plan, loop_plan, native_plan], {}, build)
        self.assertEqual(sorted([bvh, loop]), groups[0]['variants'])
        self.assertFalse(groups[0]['native_ray_queries'])
        self.assertTrue(groups[0]['persistent'])
        self.assertEqual([original], groups[1]['variants'])
        self.assertTrue(groups[1]['native_ray_queries'])
        self.assertEqual(('NativeQuery', 'TriangleLoop', 'Custom', 'HardwarePipeline', 'ComputeBvh'),
                         ray.TRAVERSAL_CODES)

    def test_invocation_type_is_independent_of_logical_ray_node_and_preserves_legacy(self):
        self.assertEqual('Compute', invocation.resolve_type(self.node))
        self.assertEqual('NativeQuery', invocation.resolve_traversal(self.node))
        self.node['invocation']['type'] = 'Compute'
        self.assertEqual('TriangleLoop', invocation.resolve_traversal(self.node))
        self.assertEqual('RayTracingPipeline', self.compile()['passes'][0]['pipeline']['kind'])
        self.node['invocation']['traversal'] = 'NativeQuery'
        self.assertTrue(ray.requires_native_queries(self.compile()))
        self.node['invocation'] = {'type': 'RayTracing', 'dimensions': 'ColorTarget'}
        plan = self.compile()
        self.assertEqual(ray.HARDWARE_PIPELINE_TAG, ray.program_tuple(self.node)[-1])
        profiles = ray.group_profiles([plan], {}, build)
        self.assertTrue(profiles[0]['native_pipeline'])
        self.assertFalse(profiles[0]['persistent'])
        self.assertTrue(ray.requires_native_queries(plan))

    def test_native_invocation_choices_share_a_callback_set_without_forcing_persistent_storage(self):
        inline = self.compile()
        self.node['invocation']['type'] = 'RayTracing'
        native = self.compile()
        profiles = ray.group_profiles([inline, native], {}, build)
        self.assertEqual(1, len(profiles))
        self.assertEqual(1, len(profiles[0]['variants']))
        self.assertFalse(profiles[0]['persistent'])
        self.assertTrue(profiles[0]['native_pipeline'])

    def test_hardware_invocation_rejects_compute_traversal_fields(self):
        self.node['invocation'].update(type='RayTracing', traversal='NativeQuery')
        with self.assertRaisesRegex(ToolingError, 'owns native traversal'):
            self.compile()
        self.node['invocation'] = {'type': 'Unknown', 'dimensions': 'ColorTarget'}
        with self.assertRaises(ToolingError):
            self.compile()

    def test_custom_traversal_is_validated_and_keeps_its_source_in_variant_identity(self):
        self.node['invocation'].update(type='Compute', traversal='Custom', traversal_source='queries/first.slang')
        plan = self.compile()
        first = ray.program_tuple(self.node)
        self.assertEqual('queries/first.slang', ray.get_custom_traversal_source(first))
        self.assertFalse(ray.requires_native_queries(plan))
        self.node['invocation']['traversal_source'] = 'queries/second.slang'
        self.assertNotEqual(first, ray.program_tuple(self.node))
        self.assertEqual(ray.callback_programs(first), ray.callback_programs(ray.program_tuple(self.node)))
        for source in ('../escape.slang', '/absolute.slang', 'query.txt', '', None, 'queries\\escape.slang'):
            with self.subTest(source=source), self.assertRaises(ToolingError):
                self.node['invocation']['traversal_source'] = source
                self.compile()
        self.node['invocation'].pop('traversal_source')
        with self.assertRaisesRegex(ToolingError, 'requires traversal_source'):
            self.compile()

    def connect_members(self, members):
        configuration = self.document['configurations'][0]
        configuration['edges'] = [edge for edge in configuration['edges'] if not edge['to'].startswith('trace.scene')]
        configuration['edges'] += [{'from': 'scene.' + part, 'to': 'trace.scene_' + part} for part in members]

    def test_scene_bundle_defaults_and_selected_members_reach_numeric_plan(self):
        from ludus_tools import render_graph_execution as execution
        operation = self.compile()['passes'][0]
        self.assertEqual(7, operation['scene_data'])
        for members, expected in ((('geometry',), 1), (('geometry', 'materials'), 3),
                                  (('geometry', 'lights'), 5), (('geometry', 'materials', 'lights'), 7)):
            with self.subTest(members=members):
                self.connect_members(members)
                plan = self.compile()
                operation = plan['passes'][0]
                self.assertEqual(expected, operation['scene_data'])
                _, _, passes, _ = execution.lower_plan({'plans': [plan]}, 'Default')
                self.assertEqual(expected, passes[0]['SceneData'])
                self.assertEqual('Main/scene', operation['scene'])

    def test_scene_component_outputs_are_values_and_never_texture_targets(self):
        self.connect_members(('geometry', 'materials'))
        configuration = self.document['configurations'][0]
        child = {'version': 2, 'id': 'Source', 'default': 'Default', 'configurations': [{
            'id': 'Default', 'inputs': [], 'nodes': [{'id': 'scene', 'kind': 'Scene'}], 'edges': [],
            'outputs': [{'id': part, 'type': 'Scene' + part.title(), 'source': 'scene.' + part, 'required': False}
                        for part in ('geometry', 'materials')]}]}
        (self.path.parent / 'source.lurender.json').write_bytes(graph.canonical_asset(child))
        configuration['nodes'] = [node if node['id'] != 'scene' else
            {'id': 'scene', 'kind': 'Subgraph', 'asset': 'source.lurender.json', 'configuration': 'Default'}
            for node in configuration['nodes']]
        self.assertEqual(3, self.compile()['passes'][0]['scene_data'])
        color = next(node for node in configuration['nodes'] if node['id'] == 'color')
        color['type'] = 'SceneGeometry'
        with self.assertRaises(ToolingError):
            self.compile()

    def test_scene_bundle_rejects_mixed_origins_and_missing_geometry(self):
        self.connect_members(('materials',))
        with self.assertRaisesRegex(ToolingError, 'Geometry'):
            self.compile()
        self.connect_members(('geometry', 'materials'))
        configuration = self.document['configurations'][0]
        configuration['nodes'].append({'id': 'other', 'kind': 'Scene'})
        configuration['edges'][-1]['from'] = 'other.materials'
        with self.assertRaisesRegex(ToolingError, 'same Scene generation'):
            self.compile()

    def test_scene_bundle_rejects_full_and_partial_wires_even_on_unused_pass(self):
        self.connect_members(('geometry',))
        configuration = self.document['configurations'][0]
        configuration['edges'].append({'from': 'scene.scene', 'to': 'trace.scene'})
        self.path.write_bytes(graph.canonical_asset(self.document))
        with self.assertRaisesRegex(ToolingError, 'full Scene'):
            graph.load_library(self.path)

    def test_scene_bundle_conflicts_join_independent_wiring_diagnostics(self):
        self.connect_members(('geometry',))
        configuration = self.document['configurations'][0]
        configuration['edges'].append({'from': 'scene.scene', 'to': 'trace.scene'})
        next(edge for edge in configuration['edges'] if edge['to'] == 'trace.view')['from'] = 'color.texture'
        with self.assertRaises(ToolingError) as caught:
            self.compile()
        issues = caught.exception.diagnostics
        self.assertEqual(['RenderGraphTypeMismatch', 'RenderGraphMultipleWriters'],
                         [issue['code'] for issue in issues])
        self.assertEqual('trace', issues[1]['location']['node'])
        self.assertEqual('scene', issues[1]['location']['port'])
        self.assertEqual('input', issues[1]['location']['direction'])
        self.assertIn('disconnect', issues[1]['repair'])

    def test_scene_members_preserve_maximum_length_bundle_names(self):
        name = 's' * 63
        self.node['inputs'][0]['id'] = name
        configuration = self.document['configurations'][0]
        configuration['edges'][0] = {'from': 'scene.geometry', 'to': 'trace.' + name + '_geometry'}
        self.assertEqual(1, self.compile()['passes'][0]['scene_data'])

    def test_malformed_scene_port_ids_report_a_schema_error(self):
        for identity in ([], {}, 1, None):
            with self.subTest(identity=identity):
                self.node['inputs'][0]['id'] = identity
                with self.assertRaises(ToolingError):
                    self.compile()

    def test_scene_bundle_reserves_derived_port_ids(self):
        self.node['inputs'].append({'id': 'scene_geometry', 'type': 'SceneGeometry', 'required': False})
        with self.assertRaisesRegex(ToolingError, 'reserved'):
            self.compile()

    def test_scene_members_forward_through_typed_subgraph_ports(self):
        import copy
        self.connect_members(('geometry', 'materials'))
        parent = self.document['configurations'][0]
        child = copy.deepcopy(self.document)
        child['id'] = 'Child'
        child_configuration = child['configurations'][0]
        child_configuration['inputs'] = [{'id': part, 'type': 'Scene' + part.title()}
                                         for part in ('geometry', 'materials')]
        for edge in child_configuration['edges']:
            if edge['from'].startswith('scene.'):
                edge['from'] = 'input.' + edge['from'].split('.')[1]
        child_configuration['nodes'] = [node for node in child_configuration['nodes'] if node['kind'] != 'Scene']
        (self.path.parent / 'child.lurender.json').write_bytes(graph.canonical_asset(child))
        parent['nodes'] = [{'id': 'scene', 'kind': 'Scene'},
                           {'id': 'child', 'kind': 'Subgraph', 'asset': 'child.lurender.json', 'configuration': 'Default'}]
        parent['edges'] = [{'from': 'scene.' + part, 'to': 'child.' + part} for part in ('geometry', 'materials')]
        plan = self.compile()
        self.assertEqual(3, plan['passes'][0]['scene_data'])
        self.assertEqual('Main/scene', plan['passes'][0]['scene'])

    def test_automatic_and_partial_ray_state_preserve_source_and_resolve_identically(self):
        self.assertEqual({}, pipeline.new_node('RayTracingPipeline', 'trace')['state'])
        for state in ({}, {'attribute_bytes': 8}, {'payload_bytes': 0}, dict(ray.STATE)):
            with self.subTest(state=state):
                self.node['state'] = state
                plan = self.compile()
                operation = next(value for value in plan['passes'] if value['kind'] == 'RayTracing')
                self.assertEqual(ray.STATE, ray.resolve_state(operation['pipeline']['state']))
                self.assertEqual(state, self.node['state'])
                self.assertEqual(self.document, json.loads(self.path.read_text()))

    def test_automatic_manifest_state_preserves_authored_recipe(self):
        for fixture in ('raytracing', 'scene_ray'):
            value = json.loads((ROOT / 'tests/sdk_consumer/rg2' / fixture / 'main.lushader.json.in')
                               .read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
            technique = next(t for t in value['techniques'] if t['id'] == ray.TECHNIQUE)
            technique['recipes'] = [{'id': 'Triangles'}]
            before = deepcopy(value)
            _, _, techniques, _ = build.validate_manifest(value)
            self.assertEqual(ray.STATE, {k: v for k, v in ray.resolve_state(
                techniques[ray.TECHNIQUE]['recipes'][0]).items() if k != 'id'})
            self.assertEqual(before, value)
            technique['recipes'][0]['attribute_bytes'] = False
            with self.assertRaises(ToolingError):
                build.validate_manifest(value)
            technique['recipes'][0]['attribute_bytes'] = 0
            with self.assertRaises(ToolingError):
                build.validate_manifest(value)

    def test_unsupported_explicit_limits_and_malformed_automatic_state_are_not_ignored(self):
        for state in ({'attribute_bytes': 0}, {'max_recursion': 2}, {'payload_bytes': 4},
                      {'attribute_bytes': 'auto'}, {'payload_bytes': True}, {'unknown': 1}):
            with self.subTest(state=state):
                self.node['state'] = state
                with self.assertRaises(ToolingError):
                    self.compile()

    def test_automatic_payload_admission_follows_the_struct_with_an_optional_lower_cap(self):
        payload = b'struct ScenePayload { float4 Values[128]; };'
        ray.validate_payload(payload, None, build)  # Larger than the former implicit 1024-byte cap.
        with self.assertRaises(ToolingError):
            ray.validate_payload(payload, 1024, build)
        ray.validate_payload(b'struct ScenePayload { float4 Values[256]; };', None, build)
        with self.assertRaises(ToolingError) as failure:
            ray.validate_payload(b'struct ScenePayload { float4 Values[257]; };', None, build)
        self.assertIn('4096-byte admission limit', failure.exception.message)
        with self.assertRaises(ToolingError):
            ray.validate_payload(b'struct ScenePayload { Texture2D<float4> Texture; };', None, build)

    def test_incomplete_ray_stage_is_diagnosed_without_rewriting_source(self):
        self.node['shaders'].pop()
        self.path.write_bytes(graph.canonical_asset(self.document))
        before = self.path.read_bytes()
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(self.path)
        self.assertEqual('RenderGraphIncompletePipeline', failure.exception.code)
        self.assertEqual(before, self.path.read_bytes())

    def test_state_launch_and_extra_resources_reject_before_execution(self):
        original = deepcopy(self.node)
        changes = (lambda node: node['state'].update(max_recursion=2),
                   lambda node: node['state'].update(payload_bytes=16),
                   lambda node: node['invocation'].update(dimensions=[1, 1, 1]),
                   lambda node: node['outputs'][0].update(load='Load'),
                   lambda node: node['inputs'].append({'id': 'extra', 'type': 'Float32', 'required': False}))
        for change in changes:
            with self.subTest(change=change):
                self.node.clear()
                self.node.update(deepcopy(original))
                change(self.node)
                with self.assertRaises(ToolingError):
                    self.compile()

    def test_target_derived_launch_tracks_extent_without_rewriting_authored_node(self):
        self.node['invocation']['dimensions'] = 'ColorTarget'
        for extent in ([97, 65], [1, 1], [1920, 1080]):
            target = next(node for node in self.document['configurations'][0]['nodes'] if node['kind'] == 'Target')
            target['extent'] = extent
            plan = self.compile()
            operation = next(value for value in plan['passes'] if value['kind'] == 'RayTracing')
            self.assertEqual([*extent, 1], operation['dimensions'])
            self.assertEqual('ColorTarget', operation['pipeline']['invocation']['dimensions'])
            self.assertEqual(self.document, json.loads(self.path.read_text()))

    def test_explicit_launch_mismatch_reports_actual_expected_and_repair(self):
        self.node['invocation']['dimensions'] = [1, 2, 1]
        with self.assertRaises(ToolingError) as failure:
            self.compile()
        self.assertEqual('RenderGraphAttachmentMismatch', failure.exception.code)
        self.assertIn('[1, 2, 1]', failure.exception.message)
        self.assertIn('[96, 64, 1]', failure.exception.message)
        self.assertIn('Main/color', failure.exception.message)
        self.assertIn('Match color target', failure.exception.message)
        self.assertEqual('invocation.dimensions', failure.exception.location['field'])
        issue = failure.exception.diagnostics[0]
        self.assertEqual('[96, 64, 1]', issue['expected'])
        self.assertEqual('[1, 2, 1]', issue['actual'])
        self.assertIn('Match color target', issue['repair'])

    def test_independent_pipeline_profile_errors_are_collected_before_resource_lowering(self):
        self.node['state']['max_recursion'] = 2
        other = deepcopy(self.node)
        other['id'] = 'other'
        config = self.document['configurations'][0]
        config['nodes'].extend([other, {'id': 'otherOutput', 'kind': 'Export', 'type': 'ColorTexture'}])
        config['edges'].extend([{**edge, 'to': 'other.' + edge['to'].split('.')[1]}
                                for edge in list(config['edges']) if edge['to'].startswith(self.node['id'] + '.')])
        config['edges'].append({'from': 'other.color', 'to': 'otherOutput.texture'})
        with self.assertRaises(ToolingError) as caught:
            self.compile()
        self.assertEqual(2, len(caught.exception.diagnostics))
        self.assertEqual({self.node['id'], 'other'}, {issue['location']['node'] for issue in caught.exception.diagnostics})
        for issue in caught.exception.diagnostics:
            self.assertEqual('RenderGraphCapability', issue['code'])
            self.assertEqual('state', issue['location']['field'])
            self.assertIn('automatic', issue['repair'])

    def test_extra_depth_declaration_diagnostic_names_port_type_and_repair(self):
        self.node['inputs'].append({'id': 'unused_depth', 'type': 'DepthTexture', 'required': False})
        with self.assertRaises(ToolingError) as failure:
            self.compile()
        self.assertEqual('RenderGraphCapability', failure.exception.code)
        self.assertIn("'unused_depth' (DepthTexture)", failure.exception.message)
        self.assertIn('input declarations', failure.exception.message)
        self.assertIn('interface inputs and parent wires', failure.exception.message)

    def test_reused_ray_subgraph_derives_size_from_each_callers_target(self):
        self.node['invocation']['dimensions'] = 'ColorTarget'
        identity = self.node['id']
        configuration = self.document['configurations'][0]
        child = {'version': 2, 'id': 'RaySubgraph', 'default': 'Default', 'configurations': [{
            'id': 'Default', 'inputs': [{'id': port['id'], 'type': port['type']} for port in self.node['inputs']],
            'outputs': [{'id': 'color', 'type': 'ColorTexture', 'source': identity + '.color', 'required': False}],
            'nodes': [deepcopy(self.node)],
            'edges': [{'from': 'input.' + port['id'], 'to': identity + '.' + port['id']}
                      for port in self.node['inputs']]}]}
        child_path = self.path.parent / 'ray.lurender.json'
        child_path.write_bytes(graph.canonical_asset(child))
        configuration['nodes'] = [node for node in configuration['nodes'] if node['id'] != identity]
        configuration['nodes'].append({'id': identity, 'kind': 'Subgraph',
                                       'asset': child_path.name, 'configuration': 'Default'})
        large = deepcopy(configuration)
        large['id'] = 'Large'
        next(node for node in large['nodes'] if node['kind'] == 'Target')['extent'] = [193, 129]
        self.document['configurations'].append(large)
        self.compile()
        for name, expected in (('Default', [96, 64, 1]), ('Large', [193, 129, 1])):
            plan = graph.compile_graph(self.path, configuration=name)
            operation = next(value for value in plan['passes'] if value['kind'] == 'RayTracing')
            self.assertEqual(expected, operation['dimensions'])
            self.assertIn('launch: ' + str(expected), graph.dump_plan(plan))
        self.assertEqual(child, json.loads(child_path.read_text()))

    def test_manifest_accepts_exact_ray_functions_and_rejects_shader_stage_mismatch(self):
        value = json.loads((FIXTURE / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        _, programs, techniques, _ = build.validate_manifest(value)
        self.assertEqual('RayLaunch', programs['Generate']['input'])
        self.assertIn(ray.TECHNIQUE, techniques)
        techniques[ray.TECHNIQUE]['miss'] = 'Closest'
        with self.assertRaises(ToolingError):
            build.validate_manifest(value)

    def test_ray_packages_reject_browser_targets_without_software_fallback(self):
        fixture = (FIXTURE / 'main.lushader.json.in').read_text()
        for profiles in ('["wgsl"]', '["glsl"]', '["wgsl","glsl"]'):
            with self.subTest(profiles=profiles), self.assertRaises(ToolingError) as raised:
                build.validate_manifest(json.loads(fixture.replace('@RG2_TEST_PROFILES@', profiles)))
            self.assertIn('Hardware raytracing requires native Metal or Vulkan', str(raised.exception))

    def test_glsl_profile_and_additional_ray_functions_are_not_silently_ignored(self):
        value = json.loads((FIXTURE / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["glsl"]'))
        with self.assertRaises(ToolingError):
            build.validate_manifest(value)
        self.node['shaders'].append({'id': 'other', 'stage': 'miss', 'program': 'OtherMiss'})
        with self.assertRaises(ToolingError):
            self.compile()

    def test_scene_callbacks_require_matching_program_interfaces_and_bounded_queries(self):
        fixture = ROOT / 'tests/sdk_consumer/rg2/scene_ray/main.lushader.json.in'
        value = json.loads(fixture.read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        _, programs, techniques, _ = build.validate_manifest(value)
        self.assertEqual('Radiance', programs['Generate']['output'])
        self.assertEqual(2, techniques[ray.TECHNIQUE]['scene_interface']['surface_queries'])
        value['programs'][-1]['output'] = 'Color'
        with self.assertRaises(ToolingError):
            build.validate_manifest(value)
        value['programs'][-1]['output'] = 'ScenePayload'
        techniques[ray.TECHNIQUE]['scene_interface']['surface_queries'] = 0
        with self.assertRaises(ToolingError):
            build.validate_manifest(value)

    def test_line_query_budget_is_optional_bounded_and_integral(self):
        fixture = ROOT / 'tests/sdk_consumer/rg2/scene_ray/main.lushader.json.in'
        value = json.loads(fixture.read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        interface = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)['scene_interface']
        # Absent means 0: existing manifests keep validating without the field.
        self.assertNotIn('line_queries', interface)
        build.validate_manifest(value)
        self.assertEqual((ray.SCENE_QUERY_LIMITS['MIN_LINE_QUERIES'], ray.SCENE_QUERY_LIMITS['MAX_LINE_QUERIES']), (0, 8))
        for accepted in (0, 1, 8):
            interface['line_queries'] = accepted
            build.validate_manifest(value)
        for rejected in (-1, 9, 1.5, True, '1', None):
            with self.subTest(line_queries=rejected):
                interface['line_queries'] = rejected
                with self.assertRaises(ToolingError) as caught:
                    build.validate_manifest(value)
                self.assertEqual(caught.exception.code, 'RenderGraphBuildCapacity')
                # The diagnostic names the budget and its admitted range, which is the repair.
                self.assertIn('line query budget', caught.exception.message)
                self.assertIn('[0,8]', caught.exception.message)

    def test_capture_visibility_budget_is_optional_and_never_below_the_interactive_budget(self):
        fixture = ROOT / 'tests/sdk_consumer/rg2/scene_ray/main.lushader.json.in'
        value = json.loads(fixture.read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        interface = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)['scene_interface']
        self.assertNotIn('capture_visibility_queries', interface)
        build.validate_manifest(value)
        maximum = ray.SCENE_QUERY_LIMITS['MAX_CAPTURE_VISIBILITY_QUERIES']
        self.assertEqual(maximum, 256)
        for accepted in (interface['visibility_queries'], maximum):
            interface['capture_visibility_queries'] = accepted
            build.validate_manifest(value)
        for rejected in (interface['visibility_queries'] - 1, maximum + 1, 1.5, True, '64'):
            with self.subTest(capture_visibility_queries=rejected):
                interface['capture_visibility_queries'] = rejected
                with self.assertRaises(ToolingError) as caught:
                    build.validate_manifest(value)
                self.assertIn('capture visibility query budget', caught.exception.message)

    def test_line_query_budget_reaches_the_wrapper_only_when_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'kernel.slang').write_text('// LineKernel\n')
            (root / 'payload.slang').write_text('struct ScenePayload { float3 Radiance; };\n')
            programs = {'Kernel': {'id': 'Kernel', 'stage': 'compute', 'source': 'kernel.slang',
                                   'function': 'LineKernel', 'output': 'Radiance'}}
            recipe = {'source': 'renderer_gpu_preview.slang'}
            interface = {'version': 1, 'payload': 'payload.slang', 'surface_queries': 1, 'visibility_queries': 0}
            wrappers = {}
            for name, line_queries in (('absent', None), ('zero', 0), ('three', 3)):
                value = dict(interface) if line_queries is None else dict(interface, line_queries=line_queries)
                wrappers[name] = ray.persistent_wrapper(programs, {'scene_interface': value}, [('Kernel',)],
                                                        build.Snapshot(root), build, recipe)
            # Absent and zero keep the zero-initialized maximum and the wrapper bytes of today.
            self.assertEqual(wrappers['absent'], wrappers['zero'])
            self.assertNotIn('MaximumLineQueries', wrappers['absent'])
            self.assertNotIn('MAX_LINE_QUERIES', wrappers['absent'])
            assignment = '    queries.MaximumLineQueries = 3;\n'
            self.assertEqual(wrappers['three'].count(assignment), 1)
            self.assertEqual(wrappers['three'].replace(assignment, '', 1), wrappers['absent'])

    def test_line_traversal_is_optional_and_names_its_choices(self):
        fixture = ROOT / 'tests/sdk_consumer/rg2/scene_ray/main.lushader.json.in'
        value = json.loads(fixture.read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        interface = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)['scene_interface']
        # Absent means the packed BVH walk (point-line design D5).
        self.assertNotIn('line_traversal', interface)
        build.validate_manifest(value)
        self.assertEqual(ray.LINE_TRAVERSALS, ('ComputeBvh', 'TriangleLoop'))
        self.assertEqual(ray.DEFAULT_LINE_TRAVERSAL, 'ComputeBvh')
        for accepted in ray.LINE_TRAVERSALS:
            interface['line_traversal'] = accepted
            build.validate_manifest(value)
        for rejected in ('NativeQuery', 'Custom', 'computebvh', '', 1, True, None):
            with self.subTest(line_traversal=rejected):
                interface['line_traversal'] = rejected
                with self.assertRaises(ToolingError) as caught:
                    build.validate_manifest(value)
                self.assertEqual(caught.exception.code, 'RenderGraphBuildSchema')
                # The diagnostic names the field and both admitted values, which is the repair.
                self.assertIn('line_traversal', caught.exception.message)
                self.assertIn("'ComputeBvh'", caught.exception.message)
                self.assertIn("'TriangleLoop'", caught.exception.message)

    def test_line_traversal_defines_the_scan_only_for_triangle_loop(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'kernel.slang').write_text('// LineKernel\n')
            (root / 'payload.slang').write_text('struct ScenePayload { float3 Radiance; };\n')
            programs = {'Kernel': {'id': 'Kernel', 'stage': 'compute', 'source': 'kernel.slang',
                                   'function': 'LineKernel', 'output': 'Radiance'}}
            recipe = {'source': 'renderer_gpu_preview.slang'}
            interface = {'version': 1, 'payload': 'payload.slang', 'surface_queries': 1, 'visibility_queries': 0,
                         'line_queries': 1}
            wrappers = {}
            for name, traversal in (('absent', None), ('bvh', 'ComputeBvh'), ('scan', 'TriangleLoop')):
                value = dict(interface) if traversal is None else dict(interface, line_traversal=traversal)
                wrappers[name] = ray.persistent_wrapper(programs, {'scene_interface': value}, [('Kernel',)],
                                                        build.Snapshot(root), build, recipe)
            # The default BVH walk adds nothing, so existing line-query packages keep their wrapper bytes.
            self.assertEqual(wrappers['absent'], wrappers['bvh'])
            definition = '#define LUDUS_SCENE_LINE_TRIANGLE_LOOP 1\n'
            self.assertNotIn('LUDUS_SCENE_LINE_TRIANGLE_LOOP', wrappers['absent'])
            self.assertEqual(wrappers['scan'].count(definition), 1)
            # It precedes the helper includes that read it.
            self.assertLess(wrappers['scan'].index(definition), wrappers['scan'].index('_ludus_ray/scene_shader.slang'))
            self.assertEqual(wrappers['scan'].replace(definition, '', 1), wrappers['absent'])

    def test_scene_pipeline_adapts_legacy_stages_without_project_helpers(self):
        value = json.loads((FIXTURE / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        technique = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)
        technique['scene_interface'] = {'version': 1, 'surface_queries': 2, 'visibility_queries': 1}
        _, programs, techniques, _ = build.validate_manifest(value)
        self.assertEqual('RayHit', programs['Closest']['input'])
        # Each stage can be replaced independently by a scene-aware callback.
        programs['Closest'].update(input='SurfaceHit', output='ScenePayload')
        build.validate_manifest(value)
        self.assertNotIn('payload', techniques[ray.TECHNIQUE]['scene_interface'])

    def test_shader_local_payload_bounds_reject_resources_and_large_matrices(self):
        ray.validate_payload(b'struct ScenePayload { float3 Radiance; };', 16, build)
        with self.assertRaises(ToolingError):
            ray.validate_payload(b'struct ScenePayload { float4x3 Transform; };', 48, build)
        with self.assertRaises(ToolingError):
            ray.validate_payload(b'struct ScenePayload { Texture2D<float4> Texture; };', 1024, build)
        with self.assertRaises(ToolingError):
            ray.validate_payload(b'struct ScenePayload { float Values[9999999999999999]; };', 1024, build)

    def test_package_build_cooks_one_ray_recipe_per_prepared_profile(self):
        # build_package must cook the ray profiles that prepare_sources grouped and linked.
        root = Path(self.temporary.name)
        source = root / 'source'
        shutil.copytree(FIXTURE, source)
        manifest = source / 'main.lushader.json'
        manifest.write_text((source / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        helper = stage_helper(root / 'sdk')
        tool = root / 'tool'
        tool.write_text('test tool identity')
        tool.with_name('tool.build.json').write_text('{}')
        *_, ray_profiles = build.prepare_sources(manifest, helper)
        self.assertTrue(ray_profiles)
        with patch.object(build, 'compile_program', compile_without_tools):
            result = build.build_package(manifest, root / 'build', helper=helper, compiler=tool, spirv_cross=tool)
        descriptor = json.loads((Path(result['package']) / 'package.json').read_text())
        recipes = [recipe for recipe in descriptor['recipes'] if recipe['technique'] == ray.TECHNIQUE]
        self.assertEqual([[list(variant) for variant in profile['variants']] for profile in ray_profiles],
                         [recipe['ray_programs'] for recipe in recipes])
        keys = {program['key'] for program in descriptor['programs']}
        self.assertTrue(all(recipe['compute'] in keys for recipe in recipes))


@unittest.skipUnless(os.environ.get('LUDUS_RG1_SLANG') and os.environ.get('LUDUS_RG1_HELPER') and os.environ.get('LUDUS_RG1_CROSS'),
                     'set LUDUS_RG1_SLANG, LUDUS_RG1_HELPER and LUDUS_RG1_CROSS for native ray compiler acceptance')
class RayCompilerAcceptance(unittest.TestCase):
    def test_native_metal_stages_are_distinct_from_inline_query_compute(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray', source)
            path = source / 'main.lurender.json'
            document = json.loads(path.read_text())
            hardware = deepcopy(document['configurations'][0])
            hardware['id'] = 'Hardware'
            node = next(item for item in hardware['nodes'] if item['kind'] == 'RayTracingPipeline')
            node['invocation']['type'] = 'RayTracing'
            document['configurations'].append(hardware)
            # One bundle combines an 8x8 compute override with independently compiled native stages.
            compute_node = next(item for item in document['configurations'][0]['nodes']
                                if item['kind'] == 'RayTracingPipeline')
            compute_node['kind'] = 'ComputePipeline'
            compute_node['state'] = {'group_size': [8, 8, 1]}
            compute_node['invocation'].update(type='Compute', traversal='NativeQuery')
            path.write_bytes(graph.canonical_asset(document))
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
            result = build.build_package(manifest, root / 'build', helper=os.environ['LUDUS_RG1_HELPER'],
                                         compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            package = Path(result['package'])
            descriptor = json.loads((package / 'package.json').read_text())
            native = next(item for item in descriptor['programs'] if item['id'].endswith('NativeStages'))
            code = (package / 'programs' / native['key'] / native['descriptor']['artifact']).read_text()
            self.assertNotIn('intersection_query<', code)
            for stage in ('LudusNativeRayGeneration', 'LudusNativeClosestHit', 'LudusNativeMiss'):
                self.assertIn('[[visible]] void ' + stage, code)
            self.assertIn('intersector.intersect(ray, scene', code)
            self.assertIn('functions[stage](packet, context)', code)
            self.assertEqual(set(range(8)), {entry['binding'] for entry in native['descriptor']['interface']['entries']})
            hardware_contents = execution._runtime_contents(str(package), 'Hardware')
            self.assertEqual('HardwarePipeline', hardware_contents['passes'][0]['Traversal'])
            self.assertIn(('NativeRayStages', 'compute', 'metal'), hardware_contents['artifacts'])
            default_contents = execution._runtime_contents(str(package), 'Default')
            self.assertNotIn(('NativeRayStages', 'compute', 'metal'), default_contents['artifacts'])
            compute_artifact = default_contents['artifacts'][('RayTracing', 'compute', 'metal')]
            self.assertEqual([8, 8, 1], compute_artifact['descriptor']['interface']['workgroup'])
            self.assertEqual([64, 1, 1], native['descriptor']['interface']['workgroup'])
            execution.emit_runtime(str(package), root / 'default.h', configuration='Default')
            self.assertNotIn('result.NativeRayStages.Artifact.HasNativeRayStages = true', (root / 'default.h').read_text())
            execution.cook_runtime(str(package), root / 'hardware.lurgrt', configuration='Hardware')
            execution.emit_runtime(str(package), root / 'hardware.h', name='Hardware', configuration='Hardware')
            self.assertIn('result.NativeRayStages.Artifact.HasNativeRayStages = true', (root / 'hardware.h').read_text())

    def test_ltc_table_reads_reach_slot_13_and_the_runtime_scene_resource_word(self):
        from test_render_graph_authoring import parse_runtime
        for reads_table in (True, False):
            with self.subTest(reads_table=reads_table), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source = root / 'source'
                shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray', source)
                value = json.loads((source / 'main.lushader.json.in').read_text()
                                   .replace('@RG2_TEST_PROFILES@', '["metal"]'))
                next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)['scene_interface']['polygonal_light_sampling'] = True
                manifest = source / 'main.lushader.json'
                manifest.write_text(json.dumps(value))
                if reads_table:
                    closest = source / 'main_shaders/closest.slang'
                    closest.write_text(closest.read_text().replace(
                        '    payload.Radiance = material.BaseColor',
                        '    payload.Radiance = LookupSpecularLtc(0.5, float3(0, 0, 1), float3(0.04)).Norm * material.BaseColor', 1))
                result = build.build_package(manifest, root / 'build', helper=os.environ['LUDUS_RG1_HELPER'],
                                             compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
                package = Path(result['package'])
                contents = execution._runtime_contents(str(package), 'Default')
                # Reflection keeps the table only when a callback reads it; the flag alone binds nothing.
                self.assertEqual(reads_table, contents['ltc_table'])
                compute = contents['artifacts'][('RayTracing', 'compute', 'metal')]
                entries = {entry['binding']: entry for entry in compute['descriptor']['interface']['entries']}
                self.assertEqual(reads_table, ray.LTC_TABLE_SLOT in entries)
                if reads_table:
                    self.assertEqual(('StorageRead', ray.LTC_TABLE_STRIDE),
                                     (entries[ray.LTC_TABLE_SLOT]['kind'], entries[ray.LTC_TABLE_SLOT]['size']))
                    # SPIRV-Cross keeps the declared slot as the Metal buffer index the renderer binds.
                    self.assertIn(b'[[buffer(13)]]', compute['bytes'])
                execution.cook_runtime(str(package), root / 'graph.lurgrt', configuration='Default')
                parsed = parse_runtime((root / 'graph.lurgrt').read_bytes())
                self.assertEqual(execution.SCENE_RESOURCE_LTC_TABLE if reads_table else 0, parsed['scene_resources'])
                execution.emit_runtime(str(package), root / 'graph.h', name='Graph', configuration='Default')
                self.assertEqual(reads_table, 'result.HasLtcTable = true;' in (root / 'graph.h').read_text())

    def test_custom_traversal_compiles_without_native_queries_and_invalidates_with_its_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray', source)
            path = source / 'main.lurender.json'
            document = json.loads(path.read_text())
            native_configuration = deepcopy(document['configurations'][0])
            native_configuration['id'] = 'Native'
            node = next(item for item in document['configurations'][0]['nodes'] if item['kind'] == 'RayTracingPipeline')
            node['invocation'].update(type='Compute', traversal='Custom', traversal_source='query.slang')
            other_configuration = deepcopy(document['configurations'][0])
            other_configuration['id'] = 'OtherCustom'
            other_node = next(item for item in other_configuration['nodes'] if item['kind'] == 'RayTracingPipeline')
            other_node['invocation']['traversal_source'] = 'alternate.slang'
            document['configurations'].extend((other_configuration, native_configuration))
            custom = source / 'query.slang'
            custom.write_text('''Hit TraceWorld(float3 originWorld, float3 directionWorld, float nearDistanceWorld,
                                      float farDistanceWorld, bool isVisibility)
{
    return TraceTriangleLoop(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility);
}
''')
            (source / 'alternate.slang').write_text('''Hit TraceWorld(float3 originWorld, float3 directionWorld,
    float nearDistanceWorld, float farDistanceWorld, bool isVisibility)
{
    Hit miss = (Hit)0;
    miss.Triangle = LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    miss.DistanceWorld = farDistanceWorld;
    return miss;
}
''')
            path.write_bytes(graph.canonical_asset(document))
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal"]'))
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            result = build.build_package(manifest, root / 'build', **options)
            package = Path(result['package'])
            descriptor = json.loads((package / 'package.json').read_text())
            self.assertEqual({False, True}, {item['native_ray_queries'] for item in descriptor['recipes']
                                            if item['technique'] == ray.TECHNIQUE})
            _, _, passes, _ = execution.lower_plan(descriptor, 'Default')
            self.assertEqual('Custom', next(item['Traversal'] for item in passes if item['Program'] == 'RayTracing'))
            _, _, other_passes, _ = execution.lower_plan(descriptor, 'OtherCustom')
            self.assertNotEqual(next(item['RayProgram'] for item in passes if item['Program'] == 'RayTracing'),
                                next(item['RayProgram'] for item in other_passes if item['Program'] == 'RayTracing'))
            stage = next(item for item in descriptor['programs'] if item['id'] == ray.SOFTWARE_COMPUTE_ID)
            self.assertNotIn('RayScene', {item['kind'] for item in stage['descriptor']['interface']['entries']})
            metal = (package / 'programs' / stage['key'] / stage['descriptor']['artifact']).read_text()
            self.assertNotIn('intersection_query<', metal)
            destination = root / 'custom.lurgrt'
            execution.cook_runtime(str(package), destination)
            self.assertEqual(execution.PACKAGE_VERSION, struct.unpack_from('<I', destination.read_bytes(), 4)[0])
            custom.write_text(custom.read_text() + '\n// Authored traversal revision.\n')
            rebuilt = build.build_package(manifest, root / 'build', **options)
            self.assertNotEqual(result['generation'], rebuilt['generation'])
            published = (root / 'build/current.json').read_bytes()
            custom.write_text('This is not a valid traversal shader.\n')
            with self.assertRaises(ToolingError):
                build.build_package(manifest, root / 'build', **options)
            self.assertEqual(published, (root / 'build/current.json').read_bytes())

    def test_shared_ray_callbacks_compile_with_both_traversals_and_pipeline_kinds(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray', source)
            path = source / 'main.lurender.json'
            document = json.loads(path.read_text())
            configuration = document['configurations'][0]
            configuration['id'] = 'Native'
            document['default'] = 'Native'
            compute_configuration = deepcopy(configuration)
            compute_configuration['id'] = 'Compute'
            node = next(node for node in compute_configuration['nodes'] if node['kind'] == 'RayTracingPipeline')
            node.update(kind='ComputePipeline', state={})
            document['configurations'] = [configuration, compute_configuration]
            path.write_bytes(graph.canonical_asset(document))
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal", "wgsl"]'))
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            result = build.build_package(manifest, root / 'build', **options)
            package = Path(result['package'])
            wrapper = next(path.read_text() for path in (package / 'programs').glob('*/wrapper.slang')
                           if 'GetGraphTraversalStrategy' in path.read_text())
            self.assertIn('#define LUDUS_SOFTWARE_RAY_TRAVERSAL 1', wrapper)
            self.assertIn('const uint strategies[] = { LUDUS_GRAPH_TRAVERSAL_TRIANGLE_LOOP }', wrapper)
            self.assertNotIn('#define LUDUS_GRAPH_COMPUTE_BVH_TRAVERSAL', wrapper)
            descriptor = json.loads((package / 'package.json').read_text())
            recipes = [recipe for recipe in descriptor['recipes'] if recipe['technique'] == ray.TECHNIQUE]
            self.assertEqual({False, True}, {recipe['native_ray_queries'] for recipe in recipes})
            software = next(recipe for recipe in recipes if not recipe['native_ray_queries'])
            self.assertEqual([['Generate', 'Miss', 'Closest', ray.TRIANGLE_LOOP_TAG]], software['ray_programs'])
            stages = [program for program in descriptor['programs'] if program['id'] == ray.SOFTWARE_COMPUTE_ID]
            self.assertTrue(stages)
            for stage in stages:
                self.assertNotIn('RayScene', {entry['kind'] for entry in stage['descriptor']['interface']['entries']})
                metal = (package / 'programs' / stage['key'] / stage['descriptor']['artifact']).read_text()
                self.assertNotIn('metal_raytracing', metal)
                self.assertNotIn('intersection_query<', metal)
            _, _, native, _ = execution.lower_plan(descriptor, 'Native')
            _, _, compute, _ = execution.lower_plan(descriptor, 'Compute')
            self.assertEqual(0, next(item['RayProgram'] for item in native if item['Program'] == 'RayTracing'))
            self.assertEqual(0, next(item['RayProgram'] for item in compute if item['Program'] == 'RayTracing'))
            self.assertEqual('TriangleLoop', next(item['Traversal'] for item in compute if item['Program'] == 'RayTracing'))
            destination = root / 'shared.lurgrt'
            execution.cook_runtime(str(package), destination, configuration='Compute')
            self.assertGreater(destination.stat().st_size, 0)
            # An explicit selector change rebuilds the wrapper without modifying any authored shader.
            shader_bytes = {path: path.read_bytes() for path in (source / 'main_shaders').glob('*.slang')}
            node['invocation']['traversal'] = 'NativeQuery'
            path.write_bytes(graph.canonical_asset(document))
            rebuilt = build.build_package(manifest, root / 'build', **options)
            rebuilt_wrapper = next(path.read_text() for path in (Path(rebuilt['package']) / 'programs').glob('*/wrapper.slang')
                                   if 'float3 EvaluateGraphRay' in path.read_text())
            self.assertNotIn('GetGraphTraversalStrategy', rebuilt_wrapper)
            self.assertEqual(shader_bytes, {path: path.read_bytes() for path in shader_bytes})

    def test_compute_bvh_and_triangle_loop_share_one_software_kernel(self):
        from test_render_graph_authoring import parse_runtime
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/traversal_parity', source)
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal", "wgsl"]'))
            result = build.build_package(manifest, root / 'build', helper=os.environ['LUDUS_RG1_HELPER'],
                                         compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            package = Path(result['package'])
            descriptor = json.loads((package / 'package.json').read_text())
            recipes = [recipe for recipe in descriptor['recipes'] if recipe['technique'] == ray.TECHNIQUE]
            software = next(recipe for recipe in recipes if not recipe['native_ray_queries'])
            callbacks = ['Generate', 'Miss', 'Closest']
            self.assertEqual([callbacks + [ray.COMPUTE_BVH_TAG], callbacks + [ray.TRIANGLE_LOOP_TAG]],
                             software['ray_programs'])
            wrapper_path = next(path for path in (package / 'programs').glob('*/wrapper.slang')
                                if 'GetGraphTraversalStrategy' in path.read_text())
            wrapper = wrapper_path.read_text()
            self.assertIn('const uint strategies[] = { LUDUS_GRAPH_TRAVERSAL_COMPUTE_BVH, '
                          'LUDUS_GRAPH_TRAVERSAL_TRIANGLE_LOOP }', wrapper)
            for define in ('LUDUS_SOFTWARE_RAY_TRAVERSAL', 'LUDUS_GRAPH_COMPUTE_BVH_TRAVERSAL'):
                self.assertIn(f'#define {define} 1', wrapper)
            for stage in (program for program in descriptor['programs'] if program['id'] == ray.SOFTWARE_COMPUTE_ID):
                self.assertNotIn('RayScene', {entry['kind'] for entry in stage['descriptor']['interface']['entries']})
                metal = (package / 'programs' / stage['key'] / stage['descriptor']['artifact']).read_text()
                self.assertNotIn('metal_raytracing', metal)
                self.assertNotIn('intersection_query<', metal)
            for configuration, index, strategy in (('ComputeBvh', 0, 'ComputeBvh'), ('TriangleLoop', 1, 'TriangleLoop'),
                                                   ('Native', 0, 'NativeQuery')):
                _, _, passes, _ = execution.lower_plan(descriptor, configuration)
                operation = next(item for item in passes if item['Program'] == 'RayTracing')
                self.assertEqual((index, strategy), (operation['RayProgram'], operation['Traversal']))
            destination = root / 'bvh.lurgrt'
            execution.cook_runtime(str(package), destination, configuration='ComputeBvh')
            self.assertEqual(ray.TRAVERSAL_CODES.index('ComputeBvh'), parse_runtime(destination.read_bytes())['passes'][0][-1])
            # The graph profile emits no browser ray recipe, but the same linked kernel already
            # compiles to WGSL with the pinned compiler: it uses no native ray-query constructs.
            tree = root / 'wgsl'
            shutil.copytree(source / 'main_shaders', tree / 'main_shaders')
            shutil.copytree(os.environ['LUDUS_RG1_HELPER'], tree / '_ludus_ray')
            shutil.copy(wrapper_path, tree / 'wrapper.slang')
            output = tree / 'kernel.wgsl'
            subprocess.run([os.environ['LUDUS_RG1_SLANG'], str(tree / 'wrapper.slang'), '-target', 'wgsl', '-entry', 'rgEntry',
                            '-stage', 'compute', '-o', str(output)], check=True, cwd=tree, capture_output=True)
            self.assertIn('@compute', output.read_text())

    def test_ray_path_capture_variant_has_its_own_identity_and_is_never_cooked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/ray_path', source)
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal"]'))
            original = {path.relative_to(source): path.read_bytes() for path in source.rglob('*') if path.is_file()}
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            plain = build.build_package(manifest, root / 'build', **options)
            plain_descriptor = json.loads((Path(plain['package']) / 'package.json').read_text())
            capture = build.build_package(manifest, root / 'build', ray_path_capture=True, **options)
            package = Path(capture['package'])
            descriptor = json.loads((package / 'package.json').read_text())
            # Only the linked native and software ray kernels are recompiled; the flag joins their
            # stage cache identity. TriangleLoop and ComputeBvh share the software kernel.
            self.assertEqual(2, capture['cache_misses'])
            recipe = next(recipe for recipe in descriptor['recipes'] if recipe['technique'] == ray.TECHNIQUE)
            self.assertTrue(recipe['ray_path_capture'])
            self.assertNotIn('ray_path_capture', next(recipe for recipe in plain_descriptor['recipes']
                                                      if recipe['technique'] == ray.TECHNIQUE))
            wrappers = [path.read_text() for path in (package / 'programs').glob('*/wrapper.slang')]
            self.assertTrue(any(text.startswith('#define LUDUS_RAY_PATH_CAPTURE 1\n') for text in wrappers))
            contents = execution._runtime_contents(package, 'Default')
            self.assertTrue(contents['ray_path_capture'])
            compute = next(artifact for (name, stage, target), artifact in contents['artifacts'].items()
                           if name == 'RayTracing' and stage == 'compute')
            entries = {entry['binding']: entry for entry in compute['descriptor']['interface']['entries']}
            self.assertEqual(('StorageReadWrite', ray.RAY_PATH_CAPTURE_STRIDE),
                             (entries[ray.RAY_PATH_CAPTURE_SLOT]['kind'], entries[ray.RAY_PATH_CAPTURE_SLOT]['size']))
            header = root / 'capture.h'
            execution.emit_runtime(package, header, name='Capture')
            self.assertIn('result.HasRayPathCapture = true;', header.read_text())
            destination = root / 'capture.lurgrt'
            with self.assertRaises(ToolingError):
                execution.cook_runtime(package, destination)
            self.assertFalse(destination.exists())
            self.assertEqual(0, build.build_package(manifest, root / 'build', **options)['cache_misses'])
            self.assertEqual(original, {path.relative_to(source): path.read_bytes() for path in source.rglob('*') if path.is_file()})

    def test_ray_path_capture_links_software_traversal_without_native_queries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/ray_path', source)
            path = source / 'main.lurender.json'
            document = json.loads(path.read_text())
            custom = deepcopy(next(item for item in document['configurations'] if item['id'] == 'TriangleLoop'))
            custom['id'] = 'Custom'
            node = next(item for item in custom['nodes'] if item['id'] == 'paths')
            node['invocation'].update(traversal='Custom', traversal_source='query.slang')
            document['configurations'].append(custom)
            path.write_bytes(graph.canonical_asset(document))
            (source / 'query.slang').write_text('''Hit TraceWorld(float3 originWorld, float3 directionWorld, float nearDistanceWorld,
                                      float farDistanceWorld, bool isVisibility)
{
    return TraceTriangleLoop(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility);
}
''')
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal"]'))
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            capture = build.build_package(manifest, root / 'build', ray_path_capture=True, **options)
            package = Path(capture['package'])
            descriptor = json.loads((package / 'package.json').read_text())
            # Software strategies share one capture kernel: the capture slot and no native ray scene.
            stage = next(item for item in descriptor['programs'] if item['id'] == ray.SOFTWARE_COMPUTE_ID)
            entries = {entry['binding']: entry for entry in stage['descriptor']['interface']['entries']}
            self.assertEqual(('StorageReadWrite', ray.RAY_PATH_CAPTURE_STRIDE),
                             (entries[ray.RAY_PATH_CAPTURE_SLOT]['kind'], entries[ray.RAY_PATH_CAPTURE_SLOT]['size']))
            self.assertNotIn('RayScene', {entry['kind'] for entry in entries.values()})
            metal = (package / 'programs' / stage['key'] / stage['descriptor']['artifact']).read_text()
            self.assertNotIn('intersection_query<', metal)
            wrapper = (package / 'programs' / stage['key'] / 'wrapper.slang').read_text()
            self.assertIn('#define LUDUS_RAY_PATH_CAPTURE 1\n', wrapper)
            self.assertIn('#define LUDUS_SOFTWARE_RAY_TRAVERSAL 1\n', wrapper)
            for configuration in ('TriangleLoop', 'ComputeBvh', 'Custom'):
                _, _, passes, _ = execution.lower_plan(descriptor, configuration)
                self.assertEqual(configuration, next(item['Traversal'] for item in passes
                                                     if item['Program'] == 'RayTracing'))
                destination = root / f'{configuration}.lurgrd'
                cooked = execution.cook_ray_path_capture(package, destination, configuration=configuration)
                self.assertEqual(struct.unpack('<2I', destination.read_bytes()[:8]),
                                 (execution.RAY_PATH_CAPTURE_PACKAGE_MAGIC, execution.PACKAGE_VERSION))
                self.assertEqual(configuration, cooked['configuration'])

    def test_capture_profile_links_the_capture_budget_and_cooks_only_as_a_capture_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray', source)
            manifest = source / 'main.lushader.json'
            value = json.loads((source / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
            interface = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)['scene_interface']
            capture_budget = 64
            interface['capture_visibility_queries'] = capture_budget
            manifest.write_text(json.dumps(value))
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            plain = build.build_package(manifest, root / 'plain', **options)
            capture = build.build_package(manifest, root / 'capture', capture_profile=True, **options)

            def wrappers(report):
                return [path.read_text() for path in (Path(report['package']) / 'programs').glob('*/wrapper.slang')]
            interactive = f"    queries.MaximumVisibilityQueries = {interface['visibility_queries']};"
            captured = f'    queries.MaximumVisibilityQueries = {capture_budget};'
            # Only the capture profile links the larger budget; interactive kernels keep visibility_queries.
            self.assertTrue(any(interactive in text for text in wrappers(plain)))
            self.assertFalse(any(captured in text for text in wrappers(plain)))
            self.assertTrue(any(captured in text for text in wrappers(capture)))
            descriptor = json.loads((Path(capture['package']) / 'package.json').read_text())
            recipe = next(recipe for recipe in descriptor['recipes'] if recipe['technique'] == ray.TECHNIQUE)
            self.assertTrue(recipe['capture_profile'])
            # A capture profile is never a runtime package, and a plain build is never a capture profile.
            with self.assertRaises(ToolingError):
                execution.cook_runtime(capture['package'], root / 'refused.lurgrt')
            with self.assertRaises(ToolingError):
                execution.cook_capture_profile(plain['package'], root / 'refused.lurgcp')
            self.assertFalse((root / 'refused.lurgrt').exists())
            destination = root / 'scene.lurgcp'
            cooked = execution.cook_capture_profile(capture['package'], destination)
            data = destination.read_bytes()
            self.assertEqual(struct.unpack_from('<I', data, 0)[0], execution.CAPTURE_PROFILE_PACKAGE_MAGIC)
            self.assertEqual(data[:4], b'LURC')
            self.assertEqual(cooked['sha256'], hashlib.sha256(data).hexdigest())
            runtime = execution.cook_runtime(plain['package'], root / 'scene.lurgrt')
            self.assertEqual(cooked['interface_identity'], runtime['interface_identity'])
            with self.assertRaises(ToolingError):
                build.build_package(manifest, root / 'both', capture_profile=True, ray_path_capture=True, **options)

    def test_capture_profile_rebuilds_a_generation_only_when_it_declares_a_larger_budget(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray', source)
            manifest = source / 'main.lushader.json'
            value = json.loads((source / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
            manifest.write_text(json.dumps(value))
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            # Without a larger capture budget the runtime package is the capture package: nothing compiles.
            plain = build.build_package(manifest, root / 'plain', **options)
            skipped = reload.rebuild_capture_profile(root / 'plain', root / 'plain-capture', root / 'plain.lurgcp',
                                                     **options)
            self.assertEqual(skipped, {'capture_profile': None, 'source_generation': plain['generation']})
            self.assertFalse((root / 'plain-capture').exists())
            self.assertFalse((root / 'plain.lurgcp').exists())

            interface = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)['scene_interface']
            interface['capture_visibility_queries'] = 64
            manifest.write_text(json.dumps(value))
            budgeted = build.build_package(manifest, root / 'build', **options)
            runtime = execution.cook_runtime(budgeted['package'], root / 'scene.lurgrt')
            # Later edits to the live sources must not reach the profile of the published generation.
            interface['capture_visibility_queries'] = interface['visibility_queries']
            manifest.write_text(json.dumps(value))
            destination = root / 'scene.lurgcp'
            result = reload.rebuild_capture_profile(root / 'build', root / 'capture', destination, **options)
            self.assertEqual(result['source_generation'], budgeted['generation'])
            self.assertEqual(destination.read_bytes()[:4], b'LURC')
            self.assertEqual(result['capture_profile']['interface_identity'], runtime['interface_identity'])
            self.assertTrue(any('    queries.MaximumVisibilityQueries = 64;' in path.read_text()
                                for path in (Path(result['build']['package']) / 'programs').glob('*/wrapper.slang')))

    def test_ray_path_capture_rebuilds_a_generation_from_its_saved_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/ray_path', source)
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal"]'))
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            plain = build.build_package(manifest, root / 'build', **options)
            runtime = execution.cook_runtime(plain['package'], root / 'plain.lurgrt')
            # Later edits to the live sources must not reach the capture of the published generation.
            edited = next((source / 'main_shaders').glob('*.slang'))
            edited.write_text(edited.read_text() + '\n// edited after publication\n')
            destination = root / 'plain.lurgrd'
            result = reload.rebuild_ray_path_capture(root / 'build', root / 'capture', destination,
                                                     root=str(source), **options)
            self.assertEqual(result['source_generation'], plain['generation'])
            capture = Path(result['build']['package'])
            self.assertEqual({path.relative_to(Path(plain['package']) / 'sources'): path.read_bytes()
                              for path in (Path(plain['package']) / 'sources').rglob('*') if path.is_file()},
                             {path.relative_to(capture / 'sources'): path.read_bytes()
                              for path in (capture / 'sources').rglob('*') if path.is_file()})
            data = destination.read_bytes()
            self.assertEqual(struct.unpack('<2I', data[:8]),
                             (execution.RAY_PATH_CAPTURE_PACKAGE_MAGIC, execution.PACKAGE_VERSION))
            self.assertEqual(result['ray_path_capture']['interface_identity'], runtime['interface_identity'])
            self.assertEqual(result['ray_path_capture']['passes'], runtime['passes'])
            self.assertTrue(execution.resource_names_path(destination).is_file())
            # A plain build is never cooked as a capture, as cook_runtime refuses capture builds.
            with self.assertRaises(ToolingError):
                execution.cook_ray_path_capture(plain['package'], root / 'refused.lurgrd')
            self.assertFalse((root / 'refused.lurgrd').exists())

    def test_builtin_visibility_templates_keep_the_static_host_interface(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            interfaces = []
            for template in ('raster', 'visibility-buffer', 'visibility-secondary-rays'):
                with self.subTest(template=template):
                    source = graph.new_workspace(root / template, output='offscreen', renderer_static=True,
                                                 profiles=('metal', 'wgsl'), template=template)
                    result = build.build_package(source.with_name('main.lushader.json'), root / (template + '-build'),
                                                 **options)
                    contents = execution._runtime_contents(result['package'], 'Default')
                    interfaces.append(contents['interface_identity'])
                    runtime = execution.cook_runtime(result['package'], root / (template + '.lurgrt'))
                    self.assertEqual(contents['interface_identity'], runtime['interface_identity'])
                    if template != 'raster':
                        raster_inputs = [len(artifact['descriptor']['interface']['inputs'])
                                         for (name, stage, target), artifact in contents['artifacts'].items()
                                         if stage == 'vertex' and name != 'RayTracing']
                        self.assertTrue(raster_inputs)
                        self.assertEqual({len(execution.VISIBILITY_INPUTS)}, set(raster_inputs))
            self.assertEqual(1, len(set(interfaces)))
            self.assertEqual('7c90fffb376e45500932936927e1dbe7eebab7e2e0b975108253787e93ace25b', interfaces[0])

    def test_visibility_and_ordinary_configurations_compile_and_cook_independently(self):
        from ludus_tools import render_graph_templates as templates, render_graph_execution as execution
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'mixed'
            source.mkdir()
            for relative, contents in templates.template_files('visibility-secondary-rays', ('metal', 'wgsl'), graph).items():
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents)
            manifest = source / 'main.lushader.json'
            value = json.loads(manifest.read_text())
            legacy = json.loads((FIXTURE / 'main.lushader.json.in').read_text()
                                .replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
            for program in legacy['programs']:
                if program['stage'] not in ray.STAGES:
                    continue
                program['id'] = 'Ordinary' + program['id']
                original = program['source']
                program['source'] = 'ordinary/' + original
                path = source / program['source']
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((FIXTURE / original).read_bytes())
                value['programs'].append(program)
            # Comments and extra payload fields must survive and work with both sets of callbacks.
            payload = source / 'main_shaders/payload.slang'
            payload.write_text('#pragma once\n// Authored payload.\nstruct ScenePayload { float3 Radiance; uint BounceCount; };\n')
            manifest.write_text(json.dumps(value))
            graph_path = source / 'main.lurender.json'
            document = json.loads(graph_path.read_text())
            ordinary = deepcopy(document['configurations'][0])
            ordinary['id'] = 'Ordinary'
            legacy_graph = json.loads((FIXTURE / 'main.lurender.json').read_text())
            trace = next(node for node in legacy_graph['configurations'][0]['nodes'] if node['kind'] == 'RayTracingPipeline')
            trace['id'] = 'transport'
            trace['invocation']['dimensions'] = 'ColorTarget'
            for shader in trace['shaders']:
                shader['program'] = 'Ordinary' + shader['program']
            ordinary['nodes'] = [trace if node['id'] == 'transport' else node for node in ordinary['nodes']
                                 if node['id'] not in ('visibilityOutput', 'depthOutput')]
            ordinary['edges'] = [edge for edge in ordinary['edges']
                                 if edge['to'].split('.')[0] not in ('visibilityOutput', 'depthOutput')]
            document['configurations'].append(ordinary)
            graph_path.write_text(json.dumps(document))
            original = {path.relative_to(source): path.read_bytes() for path in source.rglob('*') if path.is_file()}
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            result = build.build_package(manifest, root / 'build', **options)
            package = build.verify_package(Path(result['package']))
            self.assertEqual({False, True}, {recipe['visibility_buffer'] for recipe in package['recipes']
                                            if recipe['technique'] == ray.TECHNIQUE})
            for configuration, has_visibility in (('Default', True), ('Ordinary', False)):
                with self.subTest(configuration=configuration):
                    contents = execution._runtime_contents(Path(result['package']), configuration)
                    self.assertEqual(has_visibility, contents['visibility_buffer'])
                    self.assertFalse(contents['persistent_buffers'])
                    self.assertEqual(0, next(record['RayProgram'] for record in contents['passes']
                                             if record['Program'] == 'RayTracing'))
                    compute = next(artifact for (name, stage, target), artifact in contents['artifacts'].items()
                                   if name == 'RayTracing' and stage == 'compute')
                    bindings = {entry['binding'] for entry in compute['descriptor']['interface']['entries']}
                    self.assertEqual(has_visibility, ray.VISIBILITY_SLOT in bindings)
                    execution.cook_runtime(Path(result['package']), root / (configuration + '.lurgrt'), configuration=configuration)
                    execution.emit_runtime(Path(result['package']), root / (configuration + '.h'), configuration=configuration, name=configuration)
            warm = build.build_package(manifest, root / 'build', **options)
            self.assertEqual(0, warm['cache_misses'])
            self.assertEqual(original, {path.relative_to(source): path.read_bytes() for path in source.rglob('*') if path.is_file()})

    def test_old_editor_bridge_with_replaced_unguarded_callback_compiles_without_rewriting_project(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'scene_ray'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray', source)
            helper = make_legacy_scene_bridge(source)
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text()
                                .replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
            original = {p.relative_to(source): p.read_bytes() for p in source.rglob('*') if p.is_file()}
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            result = build.build_package(manifest, root / 'build', **options)
            package = build.verify_package(Path(result['package']))
            compute = next(program for program in package['programs'] if program['id'] == ray.COMPUTE_ID)
            staged = json.loads((Path(result['package']) / 'programs' / compute['key'] /
                                 'compatibility-sources.json').read_text())
            self.assertNotIn('#include "closest.slang"', staged['main_shaders/scene_legacy_callbacks.slang'])
            self.assertEqual(helper.read_bytes(), (Path(result['package']) / 'sources' /
                                                  'main_shaders/scene_legacy_callbacks.slang').read_bytes())
            self.assertEqual(original, {p.relative_to(source): p.read_bytes() for p in source.rglob('*') if p.is_file()})
            warm = build.build_package(manifest, root / 'build', **options)
            self.assertEqual(0, warm['cache_misses'])
            helper.write_text(helper.read_text() + '// Edited bridge must invalidate its compute stage.\n')
            changed = build.build_package(manifest, root / 'build', **options)
            self.assertEqual(1, changed['cache_misses'])
            current = (root / 'build/current.json').read_bytes()
            helper.write_text(helper.read_text() + '#define CUSTOM_BRIDGE 1\n')
            custom = helper.read_bytes()
            with self.assertRaises(ToolingError):
                build.build_package(manifest, root / 'build', **options)
            self.assertEqual(custom, helper.read_bytes())
            self.assertEqual(current, (root / 'build/current.json').read_bytes())

    def test_old_editor_bridge_compiles_in_ordered_ray_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'ordered'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/ordered_ray', source)
            make_legacy_scene_bridge(source)
            manifest = source / 'main.lushader.json'
            value = json.loads((source / 'main.lushader.json.in').read_text()
                               .replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
            # A legacy stage and the old bridge must share one renderer-owned RayHit.
            miss = next(program for program in value['programs'] if program['stage'] == 'miss')
            miss.update(source='main_shaders/legacy_miss.slang', function='OriginalMiss', input='Ray', output='Color')
            manifest.write_text(json.dumps(value))
            result = build.build_package(manifest, root / 'build', helper=os.environ['LUDUS_RG1_HELPER'],
                                         compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            package = build.verify_package(Path(result['package']))
            self.assertIn(ray.COMPUTE_ID, {program['id'] for program in package['programs']})

    def test_pipeline_supplies_default_payload_and_adapts_each_legacy_stage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'raytracing'
            shutil.copytree(FIXTURE, source)
            manifest = source / 'main.lushader.json'
            value = json.loads((source / 'main.lushader.json.in').read_text()
                               .replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
            technique = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)
            technique['scene_interface'] = {'version': 1, 'surface_queries': 2, 'visibility_queries': 1}
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            for is_mixed in (False, True):
                with self.subTest(is_mixed=is_mixed):
                    if is_mixed:
                        program = next(item for item in value['programs'] if item['stage'] == 'closest_hit')
                        program.update(input='SurfaceHit', output='ScenePayload')
                        (source / program['source']).write_text('''#pragma once
#include "shared.slang"
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    payload.Radiance = EvaluateMaterial(scene, hit).BaseColor;
}
''')
                        (source / program['source']).with_name('shared.slang').write_text(
                            '#pragma once\n#include "closest.slang"\n')
                    manifest.write_text(json.dumps(value))
                    result = build.build_package(manifest, root / 'build', **options)
                    package = build.verify_package(Path(result['package']))
                    self.assertEqual({'metal'}, {r['profile'] for r in package['recipes'] if r['technique'] == ray.TECHNIQUE})
                    self.assertFalse((source / 'scene_legacy_callbacks.slang').exists())
                    self.assertFalse((source / 'scene_payload.slang').exists())
                    self.assertNotIn('payload', technique['scene_interface'])

    def test_ordered_passes_adapt_legacy_callbacks_with_a_pipeline_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'ordered'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/ordered_ray', source)
            manifest = source / 'main.lushader.json'
            value = json.loads((source / 'main.lushader.json.in').read_text()
                               .replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
            technique = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)
            technique['scene_interface'].pop('payload')
            for program in value['programs']:
                if program['stage'] not in ray.STAGES:
                    continue
                # Mix scene callbacks with a legacy miss in every ordered pass.
                if program['stage'] == 'miss':
                    program.update(input='Ray', output='Color')
                    (source / program['source']).write_text('float3 ' + program['function'] + '(Ray ray) { return float3(1); }\n')
            manifest.write_text(json.dumps(value))
            result = build.build_package(manifest, root / 'build', helper=os.environ['LUDUS_RG1_HELPER'],
                                         compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            package = build.verify_package(Path(result['package']))
            self.assertEqual(1, len([p for p in package['programs'] if p['id'] == ray.COMPUTE_ID]))

    def test_sampling_in_every_callback_needs_only_a_radiance_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'scene_ray'
            shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/scene_ray', source)
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text()
                                .replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
            shaders = source / 'main_shaders'
            (shaders / 'payload.slang').write_text('struct ScenePayload { float3 Radiance; };\n')
            (shaders / 'generate.slang').write_text('''
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    uint half = SampleSceneRandom(queries, 0, 2);
    float u = SampleSceneRandom(queries);
    float v = SampleSceneRandom(queries);
    float2 jitterPixel = float2((float(half) + u) * 0.5, v);
    TraceAndShade(scene, queries, MakeCameraRay(launch, jitterPixel), payload);
    return payload.Radiance;
}
''')
            (shaders / 'closest.slang').write_text('''
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    SelectedLight selected = SelectLight(scene, queries);
    float u = SampleSceneRandom(queries);
    float v = SampleSceneRandom(queries);
    uint index = SampleSceneRandom(queries, 0, 7);
    payload.Radiance = float3(u, v, float(index) * selected.SelectionPmf);
}
''')
            (shaders / 'miss.slang').write_text('''
void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    uint index = SampleSceneRandom(queries, 1, 3);
    payload.Radiance = float3(SampleSceneRandom(queries), float(index), 0);
}
''')
            result = build.build_package(manifest, root / 'build', helper=os.environ['LUDUS_RG1_HELPER'],
                                         compiler=os.environ['LUDUS_RG1_SLANG'],
                                         spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            package = build.verify_package(Path(result['package']))
            self.assertEqual({'metal'}, {r['profile'] for r in package['recipes']
                                        if r['technique'] == ray.TECHNIQUE})
            self.assertEqual({'metal', 'wgsl'}, set(package['profiles']))
            self.assertEqual({'ray_tracing': ['metal']}, package['capability_profiles'])

    def test_automatic_limits_compile_to_concrete_state_and_retain_prior_package_on_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for fixture in ('raytracing', 'scene_ray'):
                with self.subTest(fixture=fixture):
                    source = root / fixture
                    shutil.copytree(ROOT / 'tests/sdk_consumer/rg2' / fixture, source)
                    manifest = source / 'main.lushader.json'
                    manifest.write_text((source / 'main.lushader.json.in').read_text()
                                        .replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
                    payload = source / 'main_shaders/payload.slang'
                    if fixture == 'scene_ray':
                        # The compiler sees an authored struct larger than the old implicit cap.
                        payload.write_text(payload.read_text().replace('float3 Radiance;',
                                           'float3 Radiance;\n    float4 CustomValues[128];'))
                    options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                                   spirv_cross=os.environ['LUDUS_RG1_CROSS'])
                    output = root / (fixture + '-build')
                    result = build.build_package(manifest, output, **options)
                    package = build.verify_package(Path(result['package']))
                    ray_recipes = [r for r in package['recipes'] if r['technique'] == ray.TECHNIQUE]
                    self.assertEqual({'metal'}, {r['profile'] for r in ray_recipes})
                    self.assertEqual({'metal', 'wgsl'}, set(package['profiles']))
                    self.assertEqual({'ray_tracing': ['metal']}, package['capability_profiles'])
                    self.assertEqual({'metal'}, {program['profile'] for program in package['programs']
                                                if program['id'] in (ray.COMPUTE_ID, ray.IMAGE_VERTEX_ID, ray.IMAGE_FRAGMENT_ID)})
                    self.assertTrue(all(r['state'] == {'id': 'Triangles', **ray.STATE} for r in ray_recipes))
                    # Explicit older sources and automatic sources can be mixed independently.
                    graph_file = source / 'main.lurender.json'
                    graph_value = json.loads(graph_file.read_text())
                    node = next(n for n in graph_value['configurations'][0]['nodes'] if n['kind'] == 'RayTracingPipeline')
                    node['state'] = dict(ray.STATE)
                    graph_file.write_text(json.dumps(graph_value))
                    mixed = build.build_package(manifest, output, **options)
                    mixed_package = build.verify_package(Path(mixed['package']))
                    self.assertTrue(all(r['state'] == {'id': 'Triangles', **ray.STATE}
                                        for r in mixed_package['recipes'] if r['technique'] == ray.TECHNIQUE))
                    node['state'] = {}
                    graph_file.write_text(json.dumps(graph_value))
                    manifest_value = json.loads(manifest.read_text())
                    technique = next(t for t in manifest_value['techniques'] if t['id'] == ray.TECHNIQUE)
                    technique['recipes'][0].update(ray.STATE)
                    manifest.write_text(json.dumps(manifest_value))
                    mixed = build.build_package(manifest, output, **options)
                    mixed_package = build.verify_package(Path(mixed['package']))
                    self.assertTrue(all(r['state'] == {'id': 'Triangles', **ray.STATE}
                                        for r in mixed_package['recipes'] if r['technique'] == ray.TECHNIQUE))
                    prior = (output / 'current.json').read_bytes()
                    if fixture == 'scene_ray':
                        value = json.loads(manifest.read_text())
                        technique = next(t for t in value['techniques'] if t['id'] == ray.TECHNIQUE)
                        technique['scene_interface']['local_payload_bytes'] = 1024
                        manifest.write_text(json.dumps(value))
                        with self.assertRaises(ToolingError):
                            build.build_package(manifest, output, **options)
                    else:
                        graph_file = source / 'main.lurender.json'
                        value = json.loads(graph_file.read_text())
                        node = next(n for n in value['configurations'][0]['nodes'] if n['kind'] == 'RayTracingPipeline')
                        node['state'] = {'attribute_bytes': 0}
                        graph_file.write_text(json.dumps(value))
                        with self.assertRaises(ToolingError):
                            build.build_package(manifest, output, **options)
                    self.assertEqual(prior, (output / 'current.json').read_bytes())


if __name__ == '__main__':
    unittest.main()

class OrderedRayTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        shutil.copytree(ROOT / 'tests/sdk_consumer/rg2/ordered_ray', self.source)
        self.path = self.source / 'main.lurender.json'
        self.document = json.loads(self.path.read_text())
        self.configuration = self.document['configurations'][0]

    def compile(self):
        self.path.write_bytes(graph.canonical_asset(self.document))
        return graph.compile_graph(self.path)

    def test_buffer_versions_order_distinct_callback_sets_and_retain_history(self):
        from ludus_tools import render_graph_execution as execution
        plan = self.compile()
        passes = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
        self.assertEqual(['Main/initial', 'Main/temporal', 'Main/shade'], [p['id'] for p in passes])
        self.assertEqual([(0, 1), (1, 2), (2, 3)],
                         [(p['persistent_buffers'][0]['input_version'], p['persistent_buffers'][0]['output_version']) for p in passes])
        self.assertIn('Main/initial', passes[1]['dependencies'])
        self.assertIn('Main/temporal', passes[2]['dependencies'])
        variants = [list(ray.program_tuple(p['pipeline'])) for p in reversed(passes)]
        package = {'plans': [plan], 'recipes': [{'technique': ray.TECHNIQUE, 'ray_programs': variants}]}
        _, resources, records, _ = execution.lower_plan(package, 'Default')
        self.assertEqual([2, 1, 0], [record['RayProgram'] for record in records])
        self.assertEqual(['Color', 'Buffer', 'Buffer'], [record['Kind'] for record in resources])
        self.assertTrue(all(record['Buffer0'] == 1 and record['Buffer1'] == 2 for record in records))
        self.assertTrue(all(plan['resources'][use['resource']]['initialized'] for p in passes for use in p['persistent_buffers']))
        self.assertEqual(self.document, json.loads(self.path.read_text()))

    def test_typed_job_storage_cannot_share_an_ordered_buffer_pass(self):
        buffer = next(node for node in self.configuration['nodes'] if node['kind'] == 'Buffer')
        buffer.pop('bytes')
        buffer.update(kind='StructuredBuffer', stride=16, count=1)
        with self.assertRaisesRegex(ToolingError, 'storage profiles cannot share'):
            self.compile()

    def test_persistent_writers_require_explicit_previous_version(self):
        edge = next(edge for edge in self.configuration['edges'] if edge['to'] == 'temporal.history')
        edge['from'] = 'history.buffer'
        with self.assertRaisesRegex(ToolingError, 'multiple writes branch'):
            self.compile()

    def test_same_physical_buffer_cannot_occupy_both_slots(self):
        edge = next(edge for edge in self.configuration['edges'] if edge['to'] == 'initial.reservoir')
        edge['from'] = 'history.buffer'
        with self.assertRaises(ToolingError):
            self.compile()

    def test_fixed_buffer_capacity_and_paired_load_store_are_checked(self):
        buffer = next(node for node in self.configuration['nodes'] if node['kind'] == 'Buffer')
        for size in (0, 15, 17, ray.MAX_BUFFER_BYTES + ray.BUFFER_WORD_BYTES):
            with self.subTest(size=size):
                buffer['bytes'] = size
                with self.assertRaises(ToolingError):
                    self.compile()
        buffer['bytes'] = 16
        initial = next(node for node in self.configuration['nodes'] if node['id'] == 'initial')
        initial['outputs'][1]['load'] = 'Clear'
        with self.assertRaisesRegex(ToolingError, 'Load/Store'):
            self.compile()

    @unittest.skipUnless(os.environ.get('LUDUS_RG1_SLANG') and os.environ.get('LUDUS_RG1_HELPER') and os.environ.get('LUDUS_RG1_CROSS'),
                         'requires pinned native shader tools')
    def test_distinct_node_programs_compile_and_encode_the_extended_runtime_profile(self):
        from ludus_tools import render_graph_execution as execution
        from test_render_graph_authoring import parse_runtime
        manifest = self.source / 'main.lushader.json'
        manifest.write_text((self.source / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
        result = build.build_package(manifest, self.root / 'build', helper=os.environ['LUDUS_RG1_HELPER'],
                                     compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
        destination = self.root / 'ordered.lurgrt'
        execution.cook_runtime(result['package'], destination)
        parsed = parse_runtime(destination.read_bytes())
        self.assertEqual(execution.PACKAGE_VERSION, parsed['version'])
        self.assertEqual(3, len(parsed['passes']))
        self.assertEqual({0, 1, 2}, {record[10] for record in parsed['passes']})
        self.assertEqual({(1, 2)}, {record[11:13] for record in parsed['passes']})
        self.assertEqual({(execution.ABSENT, execution.ABSENT)}, {record[13:15] for record in parsed['passes']})
        self.assertEqual({0}, {record[15] for record in parsed['passes']})
        self.assertIn(execution.PERSISTENT_RAY_STAGE_ROLE, {stage['role'] for stage in parsed['stages']})
        header = self.root / 'ordered.h'
        execution.emit_runtime(result['package'], header)
        self.assertIn('result.HasPersistentRayBuffers = true;', header.read_text())
