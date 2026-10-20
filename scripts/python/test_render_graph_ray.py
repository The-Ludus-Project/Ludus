"""Production validation for authored triangle ray graph contracts."""
from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_build as build, render_graph_ray as ray
from ludus_tools.errors import ToolingError

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


if __name__ == '__main__':
    unittest.main()
