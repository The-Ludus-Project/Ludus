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

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'tests/sdk_consumer/rg2/raytracing'


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
