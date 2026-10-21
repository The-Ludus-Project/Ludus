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
                               .read_text().replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
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

    def test_manifest_accepts_exact_ray_functions_and_rejects_shader_stage_mismatch(self):
        value = json.loads((FIXTURE / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
        _, programs, techniques, _ = build.validate_manifest(value)
        self.assertEqual('RayLaunch', programs['Generate']['input'])
        self.assertIn(ray.TECHNIQUE, techniques)
        techniques[ray.TECHNIQUE]['miss'] = 'Closest'
        with self.assertRaises(ToolingError):
            build.validate_manifest(value)

    def test_glsl_profile_and_additional_ray_functions_are_not_silently_ignored(self):
        value = json.loads((FIXTURE / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["glsl"]'))
        with self.assertRaises(ToolingError):
            build.validate_manifest(value)
        self.node['shaders'].append({'id': 'other', 'stage': 'miss', 'program': 'OtherMiss'})
        with self.assertRaises(ToolingError):
            self.compile()

    def test_scene_callbacks_require_matching_program_interfaces_and_bounded_queries(self):
        fixture = ROOT / 'tests/sdk_consumer/rg2/scene_ray/main.lushader.json.in'
        value = json.loads(fixture.read_text().replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
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


@unittest.skipUnless(os.environ.get('LUDUS_RG1_SLANG') and os.environ.get('LUDUS_RG1_HELPER'),
                     'set LUDUS_RG1_SLANG and LUDUS_RG1_HELPER for real ray compiler acceptance')
class RayCompilerAcceptance(unittest.TestCase):
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
                    options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'])
                    output = root / (fixture + '-build')
                    result = build.build_package(manifest, output, **options)
                    package = build.verify_package(Path(result['package']))
                    ray_recipes = [r for r in package['recipes'] if r['technique'] == ray.TECHNIQUE]
                    self.assertEqual({'metal', 'wgsl'}, {r['profile'] for r in ray_recipes})
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
