"""Production validation for authored triangle ray graph contracts."""
from copy import deepcopy
import json
import os
import shutil
from pathlib import Path
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_build as build, render_graph_ray as ray
from ludus_tools.errors import ToolingError
from ludus_tools import render_graph_pipeline as pipeline
from ludus_tools import render_graph_execution as execution

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'tests/sdk_consumer/rg2/raytracing'


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
        for strategy in ('Unknown', '', 1, None):
            self.node['invocation']['traversal'] = strategy
            with self.assertRaisesRegex(ToolingError, 'traversal'):
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


@unittest.skipUnless(os.environ.get('LUDUS_RG1_SLANG') and os.environ.get('LUDUS_RG1_HELPER') and os.environ.get('LUDUS_RG1_CROSS'),
                     'set LUDUS_RG1_SLANG, LUDUS_RG1_HELPER and LUDUS_RG1_CROSS for native ray compiler acceptance')
class RayCompilerAcceptance(unittest.TestCase):
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
            self.assertIn('const uint strategies[] = { 1 }', wrapper)
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
