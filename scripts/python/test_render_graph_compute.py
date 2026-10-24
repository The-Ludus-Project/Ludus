"""Scene compute passes: ComputePipeline nodes lowered into the ordered ray sequence."""
from copy import deepcopy
import json
import os
import re
import shutil
from pathlib import Path
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_build as build, render_graph_ray as ray
from ludus_tools import render_graph_compute as compute, render_graph_execution as execution
from ludus_tools.errors import ToolingError

ROOT = Path(__file__).resolve().parents[2]
SCENE_COMPUTE = ROOT / 'tests/sdk_consumer/rg2/scene_compute'
COMPUTE_MIXED = ROOT / 'tests/sdk_consumer/rg2/compute_mixed'
NATIVE_TOOLS = ('LUDUS_RG1_SLANG', 'LUDUS_RG1_HELPER', 'LUDUS_RG1_CROSS')


class SceneComputeFixture(unittest.TestCase):
    fixture = SCENE_COMPUTE

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        shutil.copytree(self.fixture, self.source)
        self.path = self.source / 'main.lurender.json'
        self.document = json.loads(self.path.read_text())
        self.configuration = self.document['configurations'][0]

    def node(self, identity):
        return next(node for node in self.configuration['nodes'] if node['id'] == identity)

    def compile(self):
        self.path.write_bytes(graph.canonical_asset(self.document))
        return graph.compile_graph(self.path)

    def fails(self, code, pattern=''):
        with self.assertRaisesRegex(ToolingError, pattern) as failure:
            self.compile()
        self.assertEqual('RenderGraph' + code, failure.exception.code)
        return failure.exception


class SceneComputeLoweringTests(SceneComputeFixture):
    def test_scene_compute_node_lowers_to_a_ray_sequence_member(self):
        before = deepcopy(self.document)
        plan = self.compile()
        member = next(operation for operation in plan['passes'] if operation['id'] == 'Main/info')
        self.assertEqual('RayTracing', member['kind'])
        self.assertEqual(compute.SOURCE_KIND, member['source_kind'])
        self.assertEqual(ray.TECHNIQUE, member['technique'])
        self.assertEqual(('SceneInfo',), ray.program_tuple(member['pipeline']))
        self.assertTrue(ray.is_compute_variant(ray.program_tuple(member['pipeline'])))
        self.assertEqual([96, 64, 1], member['dimensions'])
        # Post-processing fuses into the compute producer exactly as for a ray producer.
        self.assertEqual(2, member['accumulation']['samples'])
        self.assertEqual(1, member['tone_mapping']['exposure'])
        self.assertIn('source: ComputePipeline', graph.dump_plan(plan))
        self.assertEqual(before, json.loads(self.path.read_text()))

    def test_ray_members_keep_their_plan_records(self):
        plan = graph.compile_graph(ROOT / 'tests/sdk_consumer/rg2/ordered_ray/main.lurender.json')
        self.assertFalse(any('source_kind' in operation for operation in plan['passes']))
        self.assertNotIn('source:', graph.dump_plan(plan))

    def test_numeric_plan_keeps_the_ray_tracing_record(self):
        plan = self.compile()
        package = {'plans': [plan], 'recipes': [{'technique': ray.TECHNIQUE, 'ray_programs': [['SceneInfo']]}]}
        _, _, records, _ = execution.lower_plan(package, 'Default')
        self.assertEqual(['RayTracing'], [record['Program'] for record in records])
        self.assertEqual([0], [record['RayProgram'] for record in records])
        self.assertEqual([2], [record['AccumulationSamples'] for record in records])
        self.assertEqual([1.0], [record['ToneMappingExposure'] for record in records])

    def test_generic_compute_without_scene_keeps_the_capability_error(self):
        info = self.node('info')
        info['inputs'] = [port for port in info['inputs'] if port['type'] != 'Scene']
        self.configuration['edges'] = [edge for edge in self.configuration['edges'] if edge['to'] != 'info.scene']
        info['state'] = {'group_size': [1, 1, 1]}
        info['invocation'] = {'dimensions': [96, 64, 1]}
        error = self.fails('Capability', 'without a Scene input')
        self.assertIn('Add a Scene input', error.details['repair'])

    def test_generic_compute_drafts_still_require_explicit_group_and_dimensions(self):
        info = self.node('info')
        info['inputs'] = [port for port in info['inputs'] if port['type'] != 'Scene']
        self.configuration['edges'] = [edge for edge in self.configuration['edges'] if edge['to'] != 'info.scene']
        with self.assertRaisesRegex(ToolingError, 'group_size'):
            graph.canonical_asset(self.document)

    def test_ports_follow_the_shared_ray_contract(self):
        cases = {
            'missing view': (lambda node: node['inputs'].remove(next(p for p in node['inputs'] if p['type'] == 'View')),
                             'IncompletePipeline', 'exactly one View input'),
            'extra value input': (lambda node: node['inputs'].append({'id': 'roughness', 'type': 'Float32', 'required': False}),
                                  'Capability', 'Unsupported scene compute pipeline inputs'),
            'color load': (lambda node: node['outputs'][0].update(load='Load'),
                           'Capability', 'scene compute output must Clear/Store'),
        }
        original = deepcopy(self.document)
        for name, (change, code, pattern) in cases.items():
            with self.subTest(name):
                self.document = deepcopy(original)
                self.configuration = self.document['configurations'][0]
                info = self.node('info')
                change(info)
                if name == 'missing view':
                    self.configuration['edges'] = [e for e in self.configuration['edges'] if e['to'] != 'info.view']
                self.fails(code, pattern)

    def test_scene_compute_requires_its_compute_program(self):
        self.node('info')['shaders'] = []
        error = self.fails('IncompletePipeline', 'missing its compute shader')
        self.assertEqual('shaders', error.target['field'])

    def test_launch_size_accepts_color_target_and_the_exact_extent(self):
        info = self.node('info')
        for dimensions in ('ColorTarget', [96, 64, 1]):
            with self.subTest(dimensions=dimensions):
                info['invocation']['dimensions'] = dimensions
                self.assertEqual([96, 64, 1], next(p for p in self.compile()['passes'] if p['id'] == 'Main/info')['dimensions'])
        info['invocation']['dimensions'] = [32, 32, 1]
        error = self.fails('AttachmentMismatch', 'Scene compute launch dimensions are')
        self.assertEqual('invocation.dimensions', error.target['field'])

    def test_group_size_is_the_wrapper_workgroup(self):
        info = self.node('info')
        info['state'] = {'group_size': list(compute.GROUP_SIZE)}
        self.compile()
        info['state'] = {'group_size': [8, 8, 1]}
        before = graph.canonical_asset(self.document)
        error = self.fails('Capability', 'fixed')
        self.assertEqual('state.group_size', error.target['field'])
        self.assertIn('Clear group_size', error.details['repair'])
        self.assertEqual(before, self.path.read_bytes())
        for state in ({'group_size': [0, 1, 1]}, {'max_recursion': 1}):
            with self.subTest(state=state):
                info['state'] = state
                with self.assertRaises(ToolingError):
                    graph.canonical_asset(self.document)

    def test_post_processing_accepts_compute_producers(self):
        self.configuration['nodes'].append({'id': 'denoise', 'kind': 'Denoise', 'quality': 'Balanced'})
        edges = [edge for edge in self.configuration['edges'] if edge['to'] != 'tonemap.color']
        edges += [{'from': 'accumulate.color', 'to': 'denoise.color'}, {'from': 'denoise.color', 'to': 'tonemap.color'}]
        self.configuration['edges'] = edges
        member = next(operation for operation in self.compile()['passes'] if operation['id'] == 'Main/info')
        self.assertEqual({'id': 'Main/denoise', 'quality': 'Balanced'}, member['denoising'])
        self.assertIn('tone_mapping', member)

    def chain_compute_passes(self, count):
        """Replace the single pass by `count` consecutive scene compute passes on one color target."""
        info = self.node('info')
        nodes = [node for node in self.configuration['nodes'] if node['id'] != 'info']
        edges = [edge for edge in self.configuration['edges'] if not edge['to'].startswith('info.') and
                 not edge['from'].startswith('info.')]
        previous = 'color.texture'
        for index in range(count):
            node = deepcopy(info)
            node['id'] = f'pass{index}'
            nodes.append(node)
            edges += [{'from': 'scene.scene', 'to': node['id'] + '.scene'},
                      {'from': 'camera.view', 'to': node['id'] + '.view'},
                      {'from': previous, 'to': node['id'] + '.color'}]
            previous = node['id'] + '.color'
        edges.append({'from': previous, 'to': 'accumulate.color'})
        self.configuration['nodes'] = nodes
        self.configuration['edges'] = edges

    def test_four_pass_limit_counts_compute_members(self):
        for count, admitted in ((4, True), (5, False)):
            with self.subTest(count=count):
                self.chain_compute_passes(count)
                plan = self.compile()
                package = {'plans': [plan], 'recipes': [{'technique': ray.TECHNIQUE, 'ray_programs': [['SceneInfo']]}]}
                if admitted:
                    _, _, records, _ = execution.lower_plan(package, 'Default')
                    self.assertEqual(count, len(records))
                else:
                    with self.assertRaisesRegex(ToolingError, 'at most four triangle ray passes'):
                        execution.lower_plan(package, 'Default')
                self.setUp()


class MixedSequenceTests(SceneComputeFixture):
    fixture = COMPUTE_MIXED

    def test_compute_and_ray_members_share_one_ordered_sequence(self):
        plan = self.compile()
        members = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
        self.assertEqual(['Main/primary', 'Main/shade'], [operation['id'] for operation in members])
        self.assertEqual([compute.SOURCE_KIND, None], [operation.get('source_kind') for operation in members])
        self.assertEqual([(0, 1), (1, 2)], [(member['persistent_buffers'][0]['input_version'],
                                             member['persistent_buffers'][0]['output_version']) for member in members])
        variants = sorted(ray.program_tuple(member['pipeline']) for member in members)
        self.assertEqual([('Primary',), ('Shade', 'Miss', 'Closest')], variants)
        package = {'plans': [plan], 'recipes': [{'technique': ray.TECHNIQUE, 'ray_programs': [list(v) for v in variants]}]}
        _, _, records, _ = execution.lower_plan(package, 'Default')
        self.assertEqual([0, 1], [record['RayProgram'] for record in records])
        self.assertTrue(all(record['Buffer0'] != execution.ABSENT for record in records))

    def test_profiles_force_the_persistent_wrapper_for_compute_members(self):
        technique = {'scene_interface': {'version': 1, 'surface_queries': 1, 'visibility_queries': 0}}
        for fixture in (COMPUTE_MIXED, SCENE_COMPUTE):
            with self.subTest(fixture=fixture.name):
                plan = graph.compile_graph(fixture / 'main.lurender.json')
                profiles = ray.group_profiles([plan], technique, build)
                self.assertEqual([True], [profile['persistent'] for profile in profiles])

    def test_structured_buffer_inputs_are_rejected_naming_buffer(self):
        hits = self.node('hits')
        hits.pop('bytes')
        hits.update(kind='StructuredBuffer', stride=16, count=1)
        error = self.fails('Capability', 'replace the StructuredBuffer with a Buffer node')
        self.assertIn('Buffer node', error.details['repair'])
        self.assertEqual('primary', error.location['node'])


class ManifestTests(unittest.TestCase):
    def manifest(self, fixture):
        text = (fixture / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]')
        return json.loads(text)

    def test_compute_programs_require_the_kernel_signature_and_no_bindings(self):
        build.validate_manifest(self.manifest(SCENE_COMPUTE))
        for field, value in (('input', 'Ray'), ('output', 'ScenePayload'),
                             ('bindings', [{'id': 'data', 'schema': 'RasterVarying', 'slot': 0}])):
            with self.subTest(field=field):
                manifest = self.manifest(SCENE_COMPUTE)
                next(program for program in manifest['programs'] if program['stage'] == 'compute')[field] = value
                with self.assertRaises(ToolingError):
                    build.validate_manifest(manifest)

    def test_ray_technique_default_callbacks_are_all_or_none(self):
        manifest = self.manifest(COMPUTE_MIXED)
        build.validate_manifest(manifest)
        technique = next(item for item in manifest['techniques'] if item['id'] == ray.TECHNIQUE)
        del technique['miss']
        with self.assertRaisesRegex(ToolingError, 'all three default ray callbacks'):
            build.validate_manifest(manifest)
        for stage in ('ray_generation', 'closest_hit'):
            del technique[stage]
        build.validate_manifest(manifest)
        del technique['scene_interface']
        with self.assertRaises(ToolingError):
            build.validate_manifest(manifest)

    def test_a_technique_without_callbacks_needs_a_node_owned_pass(self):
        technique = {'scene_interface': {'version': 1, 'surface_queries': 1, 'visibility_queries': 0}}
        plan = graph.compile_graph(SCENE_COMPUTE / 'main.lurender.json', configuration='Raster')
        with self.assertRaisesRegex(ToolingError, 'without default callbacks'):
            ray.group_profiles([plan], technique, build)


@unittest.skipUnless(all(os.environ.get(name) for name in NATIVE_TOOLS), 'requires pinned native shader tools')
class NativeSceneComputeTests(unittest.TestCase):
    def build(self, fixture, root):
        source = root / fixture.name
        shutil.copytree(fixture, source)
        manifest = source / 'main.lushader.json'
        manifest.write_text((source / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
        result = build.build_package(manifest, root / (fixture.name + '-build'), helper=os.environ['LUDUS_RG1_HELPER'],
                                     compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
        package = Path(result['package'])
        wrappers = [path.read_text() for path in (package / 'programs').glob('*/wrapper.slang')]
        wrapper = next(text for text in wrappers if 'DispatchGraphRay' in text)
        return package, wrapper

    @staticmethod
    def cases(wrapper, callback_arguments):
        """Return the case labels of the pass-index switch that dispatches one callback kind."""
        match = re.search(r'switch \(GetGraphPassIndex\(\)\) \{\n((?:case \d+: [^\n]*' + re.escape(callback_arguments) +
                          r'[^\n]*\n)*)default:', wrapper)
        return re.findall(r'case (\d+):', match[1]) if match else None

    def test_compute_kernels_dispatch_only_from_ray_generation(self):
        from test_render_graph_authoring import parse_runtime
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package, wrapper = self.build(SCENE_COMPUTE, root)
            self.assertIn('case 0: return GraphSource0::ShadeSceneInfo(scene, launch, queries);', wrapper)
            self.assertIn('RWStructuredBuffer<uint4> GraphBuffer0', wrapper)
            self.assertEqual([], self.cases(wrapper, '(scene, hit, queries, payload)'))
            self.assertEqual([], self.cases(wrapper, '(scene, ray, queries, payload)'))
            descriptor = json.loads((package / 'package.json').read_text())
            recipe = next(r for r in descriptor['recipes'] if r['technique'] == ray.TECHNIQUE)
            self.assertEqual([['SceneInfo']], recipe['ray_programs'])
            self.assertTrue(recipe['persistent_buffers'])
            destination = root / 'scene_compute.lurgrt'
            execution.cook_runtime(str(package), destination)
            self.assertEqual(execution.PACKAGE_VERSION, parse_runtime(destination.read_bytes())['version'])

            _, wrapper = self.build(COMPUTE_MIXED, root)
            self.assertIn('case 0: return GraphSource2::TracePrimary(scene, launch, queries);', wrapper)
            self.assertIn('case 1: return GraphSource3::ShadeRecordedHit(scene, launch, queries);', wrapper)
            self.assertEqual(['1'], self.cases(wrapper, '(scene, hit, queries, payload)'))
            self.assertEqual(['1'], self.cases(wrapper, '(scene, ray, queries, payload)'))


if __name__ == '__main__':
    unittest.main()
