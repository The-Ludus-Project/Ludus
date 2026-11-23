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
from ludus_tools import render_graph_compute as compute, render_graph_execution as execution, shader_context
from ludus_tools.errors import ToolingError
from test_render_graph_ray import find_ray_compute

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
    def test_storage_launch_is_independent_of_viewport_extent(self):
        self.configuration = next(config for config in self.document['configurations'] if config['id'] == 'ColorInput')
        self.document['default'] = self.configuration['id']
        plan = self.compile()
        producer = next(operation for operation in plan['passes'] if operation['id'] == 'Main/utd')
        texture = plan['resources'][producer['attachments']['color']['resource']]
        self.assertEqual([8, 8, 1], producer['dimensions'])
        self.assertEqual([1, 1], texture['view_scale'])
        self.assertNotEqual(producer['dimensions'][:2], texture['extent'])
        self.node('utd')['invocation'].pop('storage_texture')
        self.node('utd')['outputs'][0]['load'] = 'Clear'
        image_plan = self.compile()
        image_producer = next(operation for operation in image_plan['passes'] if operation['id'] == 'Main/utd')
        self.assertEqual([8, 8, 1], image_producer['dimensions'])
        self.assertNotIn('storage_texture', image_producer)

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

    def test_existing_compute_kernel_can_select_triangle_loop_without_callbacks(self):
        node = self.node('info')
        node['invocation']['traversal'] = 'TriangleLoop'
        operation = next(item for item in self.compile()['passes'] if item['kind'] == 'RayTracing')
        variant = ray.program_tuple(operation['pipeline'])
        self.assertEqual(('SceneInfo', ray.TRIANGLE_LOOP_TAG), variant)
        self.assertEqual(('SceneInfo',), ray.callback_programs(variant))
        self.assertTrue(ray.is_compute_variant(variant))

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

    def test_launch_size_preserves_explicit_dimensions_in_the_numeric_plan(self):
        info = self.node('info')
        for dimensions, expected in (('ColorTarget', [96, 64, 1]), ([32, 32, 1], [32, 32, 1])):
            with self.subTest(dimensions=dimensions):
                info['invocation']['dimensions'] = dimensions
                plan = self.compile()
                member = next(p for p in plan['passes'] if p['id'] == 'Main/info')
                self.assertEqual(expected, member['dimensions'])
                package = {'plans': [plan], 'recipes': [{'technique': ray.TECHNIQUE, 'ray_programs': [['SceneInfo']]}]}
                _, resources, passes, _ = execution.lower_plan(package, 'Default')
                self.assertEqual((96, 64), (resources[passes[0]['Color']]['Width'], resources[passes[0]['Color']]['Height']))
                fixed_size = (0, 0) if dimensions == 'ColorTarget' else tuple(expected[:2])
                self.assertEqual(fixed_size, (passes[0].get('ImageLaunchWidth', 0), passes[0].get('ImageLaunchHeight', 0)))
        info['invocation']['dimensions'] = [32, 32, 2]
        self.assertEqual('invocation.dimensions', self.fails('Capability', '2D ray launch').target['field'])

    def test_group_size_is_the_wrapper_workgroup(self):
        info = self.node('info')
        info['state'] = {'group_size': list(compute.GROUP_SIZE)}
        self.compile()
        info['state'] = {'group_size': [8, 8, 1]}
        self.compile()
        info['state'] = {'group_size': [32, 2, 1]}
        before = graph.canonical_asset(self.document)
        error = self.fails('Capability', 'support')
        self.assertEqual('state.group_size', error.target['field'])
        self.assertEqual('[[64, 1, 1], [8, 8, 1]]', error.details['expected'])
        self.assertIn('Clear group_size', error.details['repair'])
        self.assertEqual(before, self.path.read_bytes())
        for state in ({'group_size': [0, 1, 1]}, {'max_recursion': 1}):
            with self.subTest(state=state):
                info['state'] = state
                with self.assertRaises(ToolingError):
                    graph.canonical_asset(self.document)

    def test_shader_bundle_inherits_one_override_and_rejects_conflicts(self):
        self.chain_compute_passes(2)
        self.node('pass0')['state'] = {'group_size': [8, 8, 1]}
        plan = self.compile()
        profiles = ray.group_profiles([plan], {}, build)
        self.assertTrue(profiles)
        self.assertTrue(all(profile['workgroup'] == [8, 8, 1] for profile in profiles))
        self.node('pass1')['state'] = {'group_size': [64, 1, 1]}
        with self.assertRaisesRegex(ToolingError, 'one workgroup shape'):
            ray.group_profiles([self.compile()], {}, build)

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

    def test_ray_pass_limit_counts_compute_members(self):
        for count, admitted in ((ray.MAX_RAY_PASSES, True), (ray.MAX_RAY_PASSES + 1, False)):
            with self.subTest(count=count):
                self.chain_compute_passes(count)
                plan = self.compile()
                package = {'plans': [plan], 'recipes': [{'technique': ray.TECHNIQUE, 'ray_programs': [['SceneInfo']]}]}
                if admitted:
                    _, _, records, _ = execution.lower_plan(package, 'Default')
                    self.assertEqual(count, len(records))
                else:
                    with self.assertRaisesRegex(ToolingError, f'at most {ray.MAX_RAY_PASSES} triangle ray passes'):
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

    def test_default_callbacks_without_a_ray_pass_link_no_visibility_inputs(self):
        # A route without ray passes links the default callbacks, which no visibility pass dispatches.
        technique = {'ray_generation': 'Generate', 'miss': 'Miss', 'closest_hit': 'Hit',
                     'scene_interface': {'version': 1, 'surface_queries': 1, 'visibility_queries': 0,
                                         'visibility_buffer': True}}
        plan = graph.compile_graph(SCENE_COMPUTE / 'main.lurender.json', configuration='Raster')
        profiles = ray.group_profiles([plan], technique, build)
        self.assertEqual([(False, True, ray.COMPUTE_ID)],
                         [(profile['visibility_buffer'], profile['native_ray_queries'], profile['compute'])
                          for profile in profiles])


@unittest.skipUnless(all(os.environ.get(name) for name in NATIVE_TOOLS), 'requires pinned native shader tools')
class NativeSceneComputeTests(unittest.TestCase):
    def build(self, fixture, root, kernel_source, *, native_output=None, image_launch=None, lit_consumer=False):
        """Build fixture and return its package, ray recipe and wrapper for the kernel compiled from kernel_source."""
        source = root / fixture.name
        shutil.copytree(fixture, source)
        manifest = source / 'main.lushader.json'
        manifest.write_text((source / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal","wgsl"]'))
        if native_output is not None:
            path = source / 'main.lurender.json'
            document = json.loads(path.read_text())
            config = next(config for config in document['configurations'] if config['id'] == 'ColorInput')
            producer = next(node for node in config['nodes'] if node['id'] == 'utd')
            producer['invocation'].pop('traversal')
            producer['invocation']['type'] = 'RayTracing'
            if lit_consumer:
                next(row for row in producer['shaders'] if row['stage'] == 'ray_generation')['program'] = 'TraceDiffraction'
            if native_output == 'image':
                producer['invocation'].pop('storage_texture')
                producer['outputs'][0]['load'] = 'Clear'
                shader = source / kernel_source
                callbacks = shader.read_text().split('void DiffractionMiss', 1)[1]
                shader.write_text('float3 ProduceDiffraction(SceneContext scene, RayLaunch launch, '
                                  'inout RayQueryContext queries, ProduceDiffractionResources resources) '
                                  '{ return float3(1, 0, 0); }\n'
                                  'void DiffractionMiss' + callbacks)
            path.write_bytes(graph.canonical_asset(document))
        if lit_consumer:
            value = json.loads(manifest.read_text())
            next(item for item in value['programs'] if item['id'] == 'ProduceDiffraction')['id'] = 'TraceDiffraction'
            technique = next(item for item in value['techniques'] if item['id'] == ray.TECHNIQUE)
            technique['scene_interface']['polygonal_light_sampling'] = True
            manifest.write_text(json.dumps(value))
            shader = source / 'main_shaders/read_diffraction.slang'
            original = shader.read_text()
            lit = original.replace('return diffraction.bgr;',
                                   'return diffraction.bgr * LookupSpecularLtc(0.5, float3(0, 0, 1), float3(0.04)).Norm;')
            self.assertNotEqual(original, lit)
            shader.write_text(lit)
        if image_launch is not None:
            path = source / 'main.lurender.json'
            document = json.loads(path.read_text())
            node = next(node for node in document['configurations'][0]['nodes'] if node['kind'] == 'ComputePipeline')
            node['invocation']['dimensions'] = image_launch
            path.write_bytes(graph.canonical_asset(document))
        result = build.build_package(manifest, root / (fixture.name + '-build'), helper=os.environ['LUDUS_RG1_HELPER'],
                                     compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
        package = Path(result['package'])
        recipe, wrapper = find_ray_compute(package, kernel_source)
        return package, recipe, wrapper

    @staticmethod
    def cases(wrapper, callback_arguments):
        """Return the case labels of the pass-index switch that dispatches one callback kind."""
        match = re.search(r'switch \(GetGraphPassIndex\(\)\) \{\n((?:case \d+: [^\n]*' + re.escape(callback_arguments) +
                          r'[^\n]*\n)*)default:', wrapper)
        return re.findall(r'case (\d+):', match[1]) if match else None

    def test_shared_buffer_stride_is_available_when_callback_compilation_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'source'
            shutil.copytree(SCENE_COMPUTE, source)
            graph_path = source / 'main.lurender.json'
            document = json.loads(graph_path.read_text())
            configuration = next(item for item in document['configurations'] if item['id'] == 'WorkItems')
            buffer = next(node for node in configuration['nodes'] if node['id'] == 'hits')
            header = 'main_shaders/edge_table.slang'
            buffer['shader_struct'] = {'source': header, 'type': 'EdgeTable', 'count': 3}
            graph_path.write_bytes(graph.canonical_asset(document))
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal"]'))
            shader = source / 'main_shaders/work_items.slang'
            shader.write_text('#include "edge_table.slang"\n' + shader.read_text()
                              + '\nuint Broken() { return undefinedIdentifier; }\n')
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'])
            output = Path(temporary) / 'build'
            (source / header).write_text('struct EdgeTable { uint Count; };\n')
            with self.assertRaisesRegex(ToolingError, 'undefinedIdentifier'):
                build.build_package(manifest, output, **options, spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            self.assertEqual(4, build.inspect_buffer_stride(manifest, header, 'EdgeTable', root=source, **options))
            (source / header).write_text('struct EdgeTable { uint4 Header; uint4 Edges[2]; };\n')
            self.assertEqual(48, build.inspect_buffer_stride(manifest, header, 'EdgeTable', root=source, **options))
            with self.assertRaisesRegex(ToolingError, 'MissingRecord'):
                build.inspect_buffer_stride(manifest, header, 'MissingRecord', root=source, **options)
            self.assertFalse((output / 'current.json').exists())

    def test_ordered_buffer_capacity_tracks_shared_slang_struct(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'source'
            shutil.copytree(SCENE_COMPUTE, source)
            graph_path = source / 'main.lurender.json'
            document = json.loads(graph_path.read_text())
            configuration = next(item for item in document['configurations'] if item['id'] == 'WorkItems')
            buffer = next(node for node in configuration['nodes'] if node['id'] == 'hits')
            header = 'main_shaders/edge_table.slang'
            buffer['shader_struct'] = {'source': header, 'type': 'EdgeTable', 'count': 1}
            graph_path.write_bytes(graph.canonical_asset(document))
            pending = graph.compile_graph(graph_path, configuration='WorkItems')
            self.assertIn('size resolved by Build Graph', graph.dump_plan(pending))
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal"]'))
            shader = source / 'main_shaders/work_items.slang'
            original = shader.read_text()
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            output = Path(temporary) / 'build'
            inline_source = 'main_shaders/work_items.slang'
            # Preview an inline struct while the Buffer is still sized in fixed bytes.
            record = buffer.pop('shader_struct')
            buffer['bytes'] = 16
            graph_path.write_bytes(graph.canonical_asset(document))
            shader.write_text('struct EdgeTable { uint4 Header; uint4 Edges[2]; };\n' + original)
            self.assertEqual(48, build.inspect_buffer_stride(
                manifest, inline_source, 'EdgeTable', root=source,
                helper=options['helper'], compiler=options['compiler']))
            del buffer['bytes']
            buffer['shader_struct'] = {**record, 'source': inline_source}
            graph_path.write_bytes(graph.canonical_asset(document))
            inline = build.build_package(manifest, output, **options)
            self.assertIn({'node': 'hits', 'graph': 'main.lurender.json', 'configuration': 'WorkItems',
                           'stride': 48, 'bytes': 48}, inline['shader_buffer_layouts'])
            shader.write_text(original)
            buffer['shader_struct']['source'] = header
            graph_path.write_bytes(graph.canonical_asset(document))
            with self.assertRaisesRegex(ToolingError, "quoted #include for shared file 'main_shaders/edge_table.slang'"):
                build.build_package(manifest, output, **options)
            shader.write_text('#include "edge_table.slang"\n' + original)
            for count in (256, 16):
                (source / header).write_text(
                    '#pragma once\nstruct EdgeRecord { float4 BeginWorldAndLength; float4 EndWorldAndCumulativeLength; };\n'
                    f'struct EdgeTable {{ uint4 Header; EdgeRecord Edges[{count}]; }};\n')
                built = build.build_package(manifest, output, **options)
                package = Path(built['package'])
                descriptor = json.loads((package / 'package.json').read_text())
                plan = next(plan for plan in descriptor['plans'] if plan['configuration'] == 'WorkItems')
                resource = plan['resources']['Main/hits']
                expected = 16 + count * 32
                self.assertEqual(expected, resource['shader_struct']['stride'])
                self.assertEqual([expected, 1], resource['extent'])
                self.assertEqual([0, 0], [entry['persistent_buffers'][0]['slot'] for entry in plan['passes']
                                          if entry['kind'] == 'RayTracing'])
                _, resources, _, _ = execution.lower_plan(descriptor, 'WorkItems')
                self.assertIn(expected, [entry['Width'] for entry in resources if entry['Kind'] == 'Buffer'])
                self.assertIn({'node': 'hits', 'graph': 'main.lurender.json', 'configuration': 'WorkItems',
                               'stride': expected, 'bytes': expected}, built['shader_buffer_layouts'])
            buffer['shader_struct']['count'] = 3
            graph_path.write_bytes(graph.canonical_asset(document))
            (source / header).write_text('struct EdgeTable { uint Count; };\n')
            built = build.build_package(manifest, output, **options)
            self.assertIn({'node': 'hits', 'graph': 'main.lurender.json', 'configuration': 'WorkItems',
                           'stride': 4, 'bytes': 16}, built['shader_buffer_layouts'])
            accepted_pointer = (output / 'current.json').read_bytes()
            (source / header).write_text('struct RenamedEdgeTable { uint4 Header; };\n')
            with self.assertRaisesRegex(ToolingError, 'EdgeTable'):
                build.build_package(manifest, output, **options)
            self.assertEqual(accepted_pointer, (output / 'current.json').read_bytes())

    def check_resource_completion(self, context, server, language, text,
                                  expression='Edge edge = resources.', expected=('Edges', 'Scratch')):
        from test_shader_context import ShaderLanguagePeer
        from urllib.parse import unquote, urlparse

        workspace = Path(unquote(urlparse(context['uri']).path)).parents[1]
        peer = ShaderLanguagePeer(server, workspace, context.get('server_arguments', ()))
        try:
            before = text[:text.index(expression) + len(expression)]
            peer.notify('textDocument/didOpen', {'textDocument': {
                'uri': context['uri'], 'languageId': language, 'version': 1, 'text': text}})
            completion = peer.call('textDocument/completion', {
                'textDocument': {'uri': context['uri']},
                'position': {'line': before.count('\n'), 'character': len(before.rsplit('\n', 1)[-1])}})
            items = completion if isinstance(completion, list) else completion.get('items', [])
            self.assertTrue(set(expected) <= {item['label'] for item in items})
        finally:
            peer.close()

    def test_lusl_typed_array_element_access_uses_reflected_edge_table_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(COMPUTE_MIXED, source)
            graph_path = source / 'main.lurender.json'
            document = json.loads(graph_path.read_text())
            configuration = document['configurations'][0]
            nodes = {node['id']: node for node in configuration['nodes']}
            nodes['hits'].pop('bytes')
            nodes['hits']['shader_struct'] = {'source': 'main_shaders/edges.luslh', 'type': 'EdgesData', 'count': 1}
            nodes['shade']['outputs'] = [port for port in nodes['shade']['outputs'] if port['id'] != 'hits']
            graph_path.write_bytes(graph.canonical_asset(document))
            manifest_path = source / 'main.lushader.json'
            validator = os.environ.get('LUDUS_RG1_VALIDATOR')
            profiles = ['metal', 'spirv'] if validator else ['metal']
            manifest = json.loads((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', json.dumps(profiles)))
            for program in manifest['programs']:
                if program['id'] in ('Primary', 'Shade'):
                    program['source'] = program['source'].replace('.slang', '.lusl')
            manifest_path.write_text(json.dumps(manifest))
            shaders = source / 'main_shaders'
            (shaders / 'edges.luslh').write_text(
                '#pragma once\n'
                'static const uint32 MAX_EDGE_COUNT = 256;\n'
                'struct Edge { float32x4 Begin; float32x3 End; float32 Weight; };\n'
                'struct EdgesData { uint32 EdgesCount; float32 TotalWeight; Edge Edges[MAX_EDGE_COUNT]; };\n')
            includes = '#include "scene_shader.luslh"\n#include "graph_resources.luslh"\n'
            producer = (
                includes +
                'float32x3 TracePrimary(in SceneContext scene, in RayLaunch launch, '
                'inout RayQueryContext inoutQueries, in TracePrimaryResources resources)\n'
                '{\n'
                '    Edge edge = resources.Hits.LoadEdges(0, 1);\n'
                '    edge.Weight = 9.0;\n'
                '    resources.Hits.StoreEdges(0, 1, edge);\n'
                '    return float32x3(0);\n'
                '}\n')
            (shaders / 'primary.lusl').write_text(producer)
            consumer = (
                includes +
                'float32x3 ShadeRecordedHit(in SceneContext scene, in RayLaunch launch, '
                'inout RayQueryContext inoutQueries, in ShadeRecordedHitResources resources)\n'
                '{\n'
                '    Edge edge = resources.Hits.LoadEdges(0, 1);\n'
                '    return float32x3(edge.Weight);\n'
                '}\n')
            shade = shaders / 'shade.lusl'
            shade.write_text(consumer)
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'], validator=validator)
            luslc = build.language.find_luslc(options['helper'])
            arrays = [{'name': 'Edges', 'element_type': 'Edge', 'capacity': 256}]
            for file, text, writable in (('primary', producer, True), ('shade', consumer, False)):
                with self.subTest(file=file):
                    context = shader_context.export_context(manifest_path, root / 'editor', f'main_shaders/{file}.lusl',
                                                            helper=options['helper'], compiler=options['compiler'])
                    field = context['resources'][0]['fields'][0]
                    self.assertEqual(arrays, field['record_arrays'])
                    self.assertEqual(writable, field['writable'])
                    unfinished = text.replace('resources.Hits.LoadEdges(0, 1)', 'resources.Hits.')
                    expected = ('LoadEdges', 'StoreEdges') if writable else ('LoadEdges',)
                    self.check_resource_completion(context, luslc, 'lusl', unfinished,
                                                   'Edge edge = resources.Hits.', expected)
            output = root / 'build'
            built = build.build_package(manifest_path, output, **options)
            self.assertIn({'node': 'hits', 'graph': 'main.lurender.json', 'configuration': configuration['id'],
                           'stride': 8208, 'bytes': 8208}, built['shader_buffer_layouts'])
            _, wrapper = find_ray_compute(Path(built['package']), 'main_shaders/primary.lusl.slang', profile='metal')
            self.assertIn('Edge LoadEdges(uint recordIndex, uint elementIndex)', wrapper)
            self.assertIn('void StoreEdges(uint recordIndex, uint elementIndex, Edge element)', wrapper)
            self.assertIn('recordIndex * 2052u + 4u + elementIndex * 8u', wrapper)
            self.assertEqual(1, wrapper.count('void StoreEdges('))
            accepted = (output / 'current.json').read_bytes()
            shade.write_text(consumer.replace('return float32x3', 'resources.Hits.StoreEdges(0, 1, edge); return float32x3'))
            with self.assertRaisesRegex(ToolingError, 'StoreEdges'):
                build.build_package(manifest_path, output, **options)
            self.assertEqual(accepted, (output / 'current.json').read_bytes())

    def test_native_edge_table_shares_the_cooked_buffer_between_passes(self):
        self.check_shared_native_edge_table('metal', is_hardware_reader=True)

    def test_ordered_compute_shares_one_native_edge_table_on_metal(self):
        self.check_shared_native_edge_table('metal')

    @unittest.skipUnless(os.environ.get('LUDUS_RG1_VALIDATOR'), 'requires pinned SPIR-V validator')
    def test_ordered_compute_shares_one_native_edge_table_on_vulkan(self):
        self.check_shared_native_edge_table('spirv')

    def test_ordered_compute_shares_one_native_edge_table_on_webgpu(self):
        self.check_shared_native_edge_table('wgsl')

    def check_shared_native_edge_table(self, target, *, is_hardware_reader=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(COMPUTE_MIXED, source)
            graph_path = source / 'main.lurender.json'
            document = json.loads(graph_path.read_text())
            configuration = document['configurations'][0]
            nodes = {node['id']: node for node in configuration['nodes']}
            nodes['hits'].pop('bytes')
            nodes['hits']['shader_struct'] = {
                'source': 'main_shaders/edges.luslh', 'type': 'EdgesData',
                'count': 1, 'access': 'Structured',
            }
            if is_hardware_reader:
                nodes['shade']['invocation']['type'] = 'RayTracing'
            if target == 'wgsl':
                for node in (nodes['primary'], nodes['shade']):
                    node['invocation']['traversal'] = 'ComputeBvh'
            nodes['shade']['outputs'] = [port for port in nodes['shade']['outputs'] if port['id'] != 'hits']
            graph_path.write_bytes(graph.canonical_asset(document))
            manifest_path = source / 'main.lushader.json'
            profiles = {'metal': ['metal', 'wgsl'], 'spirv': ['metal', 'spirv', 'wgsl'], 'wgsl': ['wgsl']}[target]
            manifest = json.loads((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', json.dumps(profiles)))
            for program in manifest['programs']:
                if program['id'] in ('Primary', 'Shade'):
                    program['source'] = program['source'].replace('.slang', '.lusl')
            manifest_path.write_text(json.dumps(manifest))
            shaders = source / 'main_shaders'
            (shaders / 'edges.luslh').write_text(
                '#pragma once\n'
                'static const uint32 MAX_EDGE_COUNT = 256;\n'
                'struct Edge { float32x4 Begin; float32x4 End; };\n'
                'struct EdgesData { uint32 EdgesCount; float32 TotalWeight; uint32x2 Padding; '
                'Edge Edges[MAX_EDGE_COUNT]; float32 Weights[MAX_EDGE_COUNT]; };\n')
            includes = '#include "scene_shader.luslh"\n#include "graph_resources.luslh"\n'
            (shaders / 'primary.lusl').write_text(includes + '''
float32x3 TracePrimary(in SceneContext scene, in RayLaunch launch,
                       inout RayQueryContext inoutQueries, in TracePrimaryResources resources)
{
    if (launch.PositionPixel.x == 0 && launch.PositionPixel.y == 0)
    {
        resources.Hits[0].Edges[0].Begin = float32x4(0.0, 1.0, 0.0, 0.0);
        resources.Hits[0].Weights[42] = 1.0;
    }
    return float32x3(0);
}
''')
            (shaders / 'shade.lusl').write_text(includes + '''
float32x3 ShadeRecordedHit(in SceneContext scene, in RayLaunch launch,
                           inout RayQueryContext inoutQueries, in ShadeRecordedHitResources resources)
{
    EdgesData edges = resources.Hits[0];
    Edge first = edges.Edges[0];
    uint32 index = SampleSceneWeightedIndex(inout inoutQueries, edges.Weights);
    return index == 42 ? first.Begin.xyz : float32x3(0);
}
''')
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            if target == 'spirv':
                options['validator'] = os.environ['LUDUS_RG1_VALIDATOR']
            context = shader_context.export_context(manifest_path, root / 'editor', 'main_shaders/shade.lusl',
                                                    helper=options['helper'], compiler=options['compiler'])
            self.assertEqual({'name': 'Hits', 'type': 'StructuredBuffer<EdgesData>', 'writable': False,
                              'native_structured': True}, context['resources'][0]['fields'][0])
            built = build.build_package(manifest_path, root / 'build', **options)
            package = Path(built['package'])
            contents = execution._runtime_contents(str(package), configuration['id'])
            writer, reader = contents['passes'][:2]
            self.assertEqual(writer['Buffer1'], reader['Buffer1'])
            self.assertNotEqual(execution.ABSENT, writer['Buffer1'])
            self.assertEqual(9232, contents['resources'][writer['Buffer1']]['Width'])
            writer_entries = contents['artifacts'][('RayTracing', 'compute', target)]['descriptor']['interface']['entries']
            reader_role = 'NativeRayStages' if is_hardware_reader else 'RecordReadCompute'
            reader_artifact = contents['artifacts'][(reader_role, 'compute', target)]
            reader_entries = reader_artifact['descriptor']['interface']['entries']
            self.assertIn((7, 'StorageReadWrite', 9232),
                          [(entry['binding'], entry['kind'], entry['size']) for entry in writer_entries])
            self.assertIn((7, 'StorageRead', 9232),
                          [(entry['binding'], entry['kind'], entry['size']) for entry in reader_entries])
            if is_hardware_reader:
                metal = reader_artifact['bytes'].decode()
                self.assertRegex(metal, r'const EdgesData_\d+ device\* GraphBuffer1_\d+ \[\[buffer\(7\)\]\]')
                self.assertNotRegex(metal, r'(?<!const )\bEdgesData_\d+ device\*')
            self.assertEqual(0 if is_hardware_reader else 1 << reader['RayProgram'], contents['record_reader_mask'])
            self.assertEqual(9232, contents['native_record_stride'])
            runtime = root / 'native-edges.lurgrt'
            execution.cook_runtime(package, runtime)
            expected_version = (execution.NATIVE_GRAPH_RECORD_PACKAGE_VERSION if is_hardware_reader else
                                execution.RECORD_READER_PACKAGE_VERSION)
            self.assertEqual(expected_version, int.from_bytes(runtime.read_bytes()[4:8], 'little'))
            if not is_hardware_reader:
                header = root / 'native-edges.h'
                execution.emit_runtime(package, header, configuration=configuration['id'])
                self.assertIn(f"result.RecordReaderMask = {1 << reader['RayProgram']};", header.read_text())
            if target == 'wgsl':
                fields = next(entry['fields'] for entry in reader_entries if entry['binding'] == 7)
                self.assertEqual([('EdgesCount', 0), ('TotalWeight', 4), ('Padding', 8),
                                  ('Edges', 16), ('Weights', 8208)],
                                 [(field['name'], field['offset']) for field in fields])
                self.assertEqual((256, 32), (fields[3]['count'], fields[3]['stride']))
                self.assertEqual((256, 4), (fields[4]['count'], fields[4]['stride']))
                self.assertEqual(['wgsl'], contents['package']['capability_profiles']['ray_tracing'])
                accepted = (root / 'build/current.json').read_bytes()
                nodes['shade']['invocation']['traversal'] = 'NativeQuery'
                graph_path.write_bytes(graph.canonical_asset(document))
                with self.assertRaisesRegex(ToolingError, 'WebGPU scene compute.*software traversal'):
                    build.build_package(manifest_path, root / 'build', **options)
                self.assertEqual(accepted, (root / 'build/current.json').read_bytes())
            if target == 'spirv':
                self.assertIn(('RecordReadCompute', 'compute', 'metal'), contents['artifacts'])
                schemas = [schema for schema in contents['package']['schemas'] if schema['id'] == 'RendererStatic']
                metal_stages = {key: item for key, item in contents['artifacts'].items() if key[2] != 'spirv'}
                interface = {'version': 2, 'scene_shader': 'SceneShaderInputsV1',
                             **execution._cpu_vertex_interface(schemas[0], metal_stages),
                             'uniform_schemas': contents['uniform_interface']}
                self.assertEqual(build.identity(interface), contents['interface_identity'])

    def test_named_edge_records_cross_compute_and_ray_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(COMPUTE_MIXED, source)
            graph_path = source / 'main.lurender.json'
            document = json.loads(graph_path.read_text())
            configuration = document['configurations'][0]
            nodes = {node['id']: node for node in configuration['nodes']}
            nodes['hits'].pop('bytes')
            nodes['hits']['shader_struct'] = {'source': 'main_shaders/edge.slang', 'type': 'Edge', 'count': 16}
            for identity in ('primary', 'shade'):
                next(port for port in nodes[identity]['inputs'] if port['id'] == 'hits')['id'] = 'edges'
                if identity == 'primary':
                    next(port for port in nodes[identity]['outputs'] if port['id'] == 'hits')['target'] = 'edges'
                else:
                    nodes[identity]['outputs'] = [port for port in nodes[identity]['outputs'] if port['id'] != 'hits']
            nodes['primary']['inputs'] = [port for port in nodes['primary']['inputs']
                                           if port['type'] in ('Scene', 'Buffer')]
            nodes['primary']['outputs'] = [port for port in nodes['primary']['outputs'] if port['type'] == 'Buffer']
            nodes['primary']['invocation'] = {'work_items': 16, 'traversal': 'ComputeBvh'}
            configuration['edges'] = [edge for edge in configuration['edges']
                                      if edge['to'] not in ('primary.view', 'primary.color')]
            for edge in configuration['edges']:
                if edge['to'] in ('primary.hits', 'shade.hits'):
                    edge['to'] = edge['to'].replace('.hits', '.edges')
                if edge['from'] == 'primary.color':
                    edge['from'] = 'color.texture'
            configuration['nodes'].append({'id': 'scratch', 'kind': 'Buffer', 'bytes': 16})
            nodes['shade']['inputs'].insert(0, {'id': 'scratch', 'type': 'Buffer', 'required': True})
            nodes['shade']['outputs'].append({'id': 'scratch', 'type': 'Buffer', 'target': 'scratch',
                                              'format': 'Bytes', 'load': 'Load', 'store': 'Store'})
            configuration['edges'].append({'from': 'scratch.buffer', 'to': 'shade.scratch'})
            graph_path.write_bytes(graph.canonical_asset(document))
            manifest_path = source / 'main.lushader.json'
            validator = os.environ.get('LUDUS_RG1_VALIDATOR')
            profiles = ['metal', 'spirv'] if validator else ['metal']
            manifest = json.loads((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', json.dumps(profiles)))
            manifest_path.write_text(json.dumps(manifest))
            shaders = source / 'main_shaders'
            (shaders / 'edge.slang').write_text(
                '#pragma once\nstruct Edge { float3 BeginWorld; float3 EndWorld; };\n')
            (shaders / 'primary.slang').write_text(
                'void StoreEndX(in GraphWritableEdgeView edges, uint index, float value) {\n'
                '    edges.StoreWord(index * 32 + 16, asuint(value));\n}\n'
                'float3 TracePrimary(SceneContext scene, RayLaunch launch, inout RayQueryContext queries, '
                'TracePrimaryResources resources) {\n'
                '    uint index = launch.WorkItemIndex;\n'
                '    if (index >= resources.Edges.GetCount()) return float3(0);\n'
                '    Edge edge; edge.BeginWorld = float3(index); edge.EndWorld = float3(index + 1);\n'
                '    resources.Edges[index] = edge;\n'
                '    StoreEndX(resources.Edges, index, float(index + 2));\n'
                '    return float3(0);\n}\n')
            consumer = (
                'float3 ShadeRecordedHit(SceneContext scene, RayLaunch launch, inout RayQueryContext queries, '
                'ShadeRecordedHitResources resources) {\n'
                '    Edge edge = resources.Edges[0];\n'
                '    if (resources.Edges.GetByteSize() < 32) return float3(0);\n'
                '    float endX = asfloat(resources.Edges.LoadWord(16));\n'
                '    return float3(endX, edge.EndWorld.yz) - edge.BeginWorld;\n}\n')
            shade = shaders / 'shade.slang'
            shade.write_text(consumer)
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ['LUDUS_RG1_CROSS'], validator=validator)
            legacy = ('float3 ShadeRecordedHit(SceneContext scene, RayLaunch launch, inout RayQueryContext queries) '
                      '{ return float3(0); }\n')
            shade.write_text(legacy)
            pending = shader_context.export_context(manifest_path, root / 'editor', 'main_shaders/shade.slang',
                                                    helper=options['helper'])['resources'][0]
            self.assertTrue(pending['needs_upgrade'])
            edges_api = {'name': 'Edges', 'type': 'Edge', 'writable': False, 'byte_access': True}
            self.assertEqual(edges_api, pending['fields'][0])
            self.assertEqual(legacy, shade.read_text())
            with self.assertRaisesRegex(ToolingError, 'Add Buffer Access') as failure:
                build.build_package(manifest_path, root / 'build', **options)
            self.assertEqual('main_shaders/shade.slang', failure.exception.location['source'])
            self.assertEqual('add_buffer_access', failure.exception.location['repair'])
            shade.write_text(consumer)
            context = shader_context.export_context(manifest_path, root / 'editor', 'main_shaders/shade.slang',
                                                    helper=options['helper'])
            self.assertIn('GraphReadOnlyEdgeView Edges;', context['prefix'])
            self.assertEqual(edges_api, context['resources'][0]['fields'][0])
            self.assertIn('Layout-dependent accessors are placeholders', context['prefix'])
            self.assertIn('uint LoadWord(uint byteOffset)', context['prefix'])
            slangd = Path(options['compiler']).with_name('slangd.exe' if os.name == 'nt' else 'slangd')
            if slangd.is_file():
                text = context['prefix'] + consumer.replace('resources.Edges[0]', 'resources.') + context['suffix']
                self.check_resource_completion(context, slangd, 'slang', text)
            output = root / 'build'
            built = build.build_package(manifest_path, output, **options)
            package = Path(built['package'])
            _, wrapper = find_ray_compute(package, 'main_shaders/primary.slang', profile='metal')
            self.assertIn('#include "main_shaders/edge.slang"', wrapper)
            self.assertIn('GraphWritableEdgeView Edges;', wrapper)
            self.assertIn('GraphReadOnlyEdgeView Edges;', wrapper)
            self.assertIn('GraphWritableEdgeView(GraphBuffer0, 16u)', wrapper)
            self.assertIn('GraphReadOnlyEdgeView(GraphBuffer1, 16u)', wrapper)
            self.assertIn('index * 8u', wrapper)
            self.assertIn('uint GetByteSize() { return Count * 32u; }', wrapper)
            self.assertNotIn('Layout-dependent accessors are placeholders', wrapper)
            execution.cook_runtime(str(package), root / 'named.lurgrt')
            nodes['shade']['inputs'].append(nodes['shade']['inputs'].pop(0))
            graph_path.write_bytes(graph.canonical_asset(document))
            rebuilt = build.build_package(manifest_path, output, **options)
            _, reordered = find_ray_compute(Path(rebuilt['package']), 'main_shaders/primary.slang', profile='metal')
            self.assertIn('GraphReadOnlyEdgeView(GraphBuffer0, 16u)', reordered)
            self.assertEqual(consumer, shade.read_text())

            # Bind the same typed read-only port through the native Metal ray-generation stage.
            shade_invocation = nodes['shade']['invocation']
            shade_invocation['type'] = 'RayTracing'
            graph_path.write_bytes(graph.canonical_asset(document))
            built = build.build_package(manifest_path, output, **options)
            package = Path(built['package'])
            contents = execution._runtime_contents(str(package), configuration['id'])
            producer, native_pass = contents['passes']
            self.assertEqual('HardwarePipeline', native_pass['Traversal'])
            self.assertEqual(producer['Buffer0'], native_pass['Buffer0'])
            self.assertNotEqual(execution.ABSENT, native_pass['Buffer0'])
            native_stage = contents['artifacts'][('NativeRayStages', 'compute', 'metal')]
            entries = native_stage['descriptor']['interface']['entries']
            self.assertIn((6, 'StorageReadWrite', 16),
                          [(entry['binding'], entry['kind'], entry['size']) for entry in entries])
            native_pipeline = next(recipe['native_pipeline'] for recipe in contents['package']['recipes']
                                   if recipe['profile'] == 'metal' and 'native_pipeline' in recipe)
            native_wrapper = (package / 'programs' / native_pipeline / 'wrapper.slang').read_text()
            self.assertIn('GraphReadOnlyEdgeView(GraphBuffer0, 16u)', native_wrapper)
            self.assertIn('LudusNativeRayGeneration', native_wrapper)
            execution.cook_runtime(package, root / 'native-named.lurgrt')
            accepted_native = (output / 'current.json').read_bytes()

            binding_location = {'source': 'main.lurender.json', 'configuration': configuration['id'],
                                'node': 'shade', 'instance': 'Main/shade', 'port': 'edges', 'direction': 'input',
                                'field': 'invocation.type'}
            for unsupported_profile in ('spirv', 'wgsl'):
                with self.subTest(unsupported_profile=unsupported_profile):
                    manifest_path.write_text(json.dumps({**manifest, 'profiles': [unsupported_profile]}))
                    with self.assertRaisesRegex(ToolingError, r"Main/shade.*'edges'.*Hardware RT pipeline.*Metal") as failure:
                        build.build_package(manifest_path, output, **options)
                    self.assertEqual(binding_location, failure.exception.location)
                    self.assertEqual(accepted_native, (output / 'current.json').read_bytes())
            manifest_path.write_text(json.dumps(manifest))

            # A record declared in the compute producer is outside the native stage bundle.
            primary = shaders / 'primary.slang'
            producer_source = primary.read_text()
            primary.write_text((shaders / 'edge.slang').read_text() + producer_source)
            nodes['hits']['shader_struct']['source'] = 'main_shaders/primary.slang'
            graph_path.write_bytes(graph.canonical_asset(document))
            with self.assertRaisesRegex(
                    ToolingError, r"Main/shade.*'edges'.*Hardware RT pipeline.*Edge.*shared .slang") as failure:
                build.build_package(manifest_path, output, **options)
            self.assertEqual(binding_location, failure.exception.location)
            self.assertEqual(accepted_native, (output / 'current.json').read_bytes())
            primary.write_text(producer_source)
            nodes['hits']['shader_struct']['source'] = 'main_shaders/edge.slang'

            del shade_invocation['type']
            graph_path.write_bytes(graph.canonical_asset(document))
            accepted = (output / 'current.json').read_bytes()
            shade.write_text(consumer.replace('float endX', 'resources.Edges[0] = edge; float endX'))
            with self.assertRaisesRegex(ToolingError, 'set|l-value|assign'):
                build.build_package(manifest_path, output, **options)
            self.assertEqual(accepted, (output / 'current.json').read_bytes())
            shade.write_text(consumer.replace('float endX', 'resources.Edges.StoreWord(16, 0); float endX'))
            with self.assertRaisesRegex(ToolingError, 'StoreWord'):
                build.build_package(manifest_path, output, **options)
            self.assertEqual(accepted, (output / 'current.json').read_bytes())

            shade.write_text(consumer)
            primary = shaders / 'primary.slang'
            producer = primary.read_text()
            nodes['hits']['shader_struct']['source'] = 'main_shaders/primary.slang'
            graph_path.write_bytes(graph.canonical_asset(document))
            primary.write_text((shaders / 'edge.slang').read_text() + producer)
            built = build.build_package(manifest_path, output, **options)
            _, inline_wrapper = find_ray_compute(Path(built['package']), 'main_shaders/primary.slang', profile='metal')
            self.assertRegex(inline_wrapper, r'typedef GraphSource\d+::Edge Edge;')
            self.assertIn('index * 8u', inline_wrapper)
            primary.write_text(producer)

            # A LUSL header supplies the storage layout to both shader languages.
            nodes['hits']['shader_struct']['source'] = 'main_shaders/edge.luslh'
            (shaders / 'edge.luslh').write_text(
                '#pragma once\n#include "math.luslh"\n#include "endpoints.luslh"\n'
                'struct Edge { EDGE_VECTOR BeginWorld; EDGE_VECTOR EndWorld; };\n')
            endpoints = shaders / 'endpoints.luslh'
            endpoints.write_text('#pragma once\n#define EDGE_VECTOR float32x3\n')
            graph_path.write_bytes(graph.canonical_asset(document))
            built = build.build_package(manifest_path, output, **options)
            self.assertIn({'node': 'hits', 'graph': 'main.lurender.json', 'configuration': configuration['id'],
                           'stride': 32, 'bytes': 512}, built['shader_buffer_layouts'])
            _, wrapper = find_ray_compute(Path(built['package']), 'main_shaders/primary.slang', profile='metal')
            self.assertIn('#include "main_shaders/edge.luslh.slang"', wrapper)
            self.assertIn('index * 8u', wrapper)
            accepted = (output / 'current.json').read_bytes()
            endpoints.write_text('#pragma once\n#define EDGE_VECTOR MissingVector\n')
            with self.assertRaisesRegex(ToolingError, 'MissingVector'):
                build.build_package(manifest_path, output, **options)
            self.assertEqual(accepted, (output / 'current.json').read_bytes())
            endpoints.write_text('#pragma once\n#define EDGE_VECTOR float32x3\n')

            for program in manifest['programs']:
                if program['id'] in ('Primary', 'Shade'):
                    program['source'] = program['source'].replace('.slang', '.lusl')
            manifest_path.write_text(json.dumps(manifest))
            (shaders / 'primary.lusl').write_text(
                '#include "scene_shader.luslh"\n#include "graph_resources.luslh"\n'
                'float32x3 TracePrimary(in SceneContext scene, in RayLaunch launch, '
                'inout RayQueryContext inoutQueries, in TracePrimaryResources resources)\n'
                '{\n'
                '    uint32 index = launch.WorkItemIndex;\n'
                '    if (index < resources.Edges.GetCount())\n'
                '    {\n'
                '        Edge edge = 0;\n'
                '        edge.BeginWorld = float32x3(float32(index));\n'
                '        edge.EndWorld = edge.BeginWorld + 1;\n'
                '        resources.Edges.Store(index, edge);\n'
                '        resources.Edges.StoreWord(index * 32 + 16, resources.Edges.LoadWord(index * 32 + 16));\n'
                '    }\n'
                '    return float32x3(0);\n'
                '}\n')
            lusl_consumer = (
                '#include "scene_shader.luslh"\n#include "graph_resources.luslh"\n'
                'float32x3 ShadeRecordedHit(in SceneContext scene, in RayLaunch launch, '
                'inout RayQueryContext inoutQueries, in ShadeRecordedHitResources resources)\n'
                '{\n'
                '    Edge edge = resources.Edges.Load(0);\n'
                '    if (resources.Edges.GetByteSize() < 32 || resources.Edges.LoadWord(16) == 0)\n'
                '    {\n'
                '        return float32x3(0);\n'
                '    }\n'
                '    if (resources.Scratch.GetCount() > 0)\n'
                '    {\n'
                '        uint32x4 word = resources.Scratch.Load(0);\n'
                '        word.x = word.x + 1;\n'
                '        resources.Scratch.Store(0, word);\n'
                '    }\n'
                '    return edge.EndWorld - edge.BeginWorld;\n'
                '}\n')
            shade_lusl = shaders / 'shade.lusl'
            legacy_lusl = lusl_consumer.replace('#include "graph_resources.luslh"\n', '').replace(
                ', in ShadeRecordedHitResources resources', '').replace('resources.Edges.Load(0)', '0')
            shade_lusl.write_text(legacy_lusl)
            pending = shader_context.export_context(manifest_path, root / 'lusl-editor', 'main_shaders/shade.lusl',
                                                    helper=options['helper'], compiler=options['compiler'])['resources'][0]
            self.assertTrue(pending['needs_upgrade'])
            self.assertEqual(edges_api, pending['fields'][0])
            self.assertEqual(legacy_lusl, shade_lusl.read_text())
            shade_lusl.write_text(lusl_consumer.replace('resources.Edges.Load(0)', 'resources.'))
            context = shader_context.export_context(manifest_path, root / 'lusl-editor', 'main_shaders/shade.lusl',
                                                    helper=options['helper'], compiler=options['compiler'])
            self.assertEqual(edges_api, context['resources'][0]['fields'][0])
            from urllib.parse import unquote, urlparse
            staged_source = Path(unquote(urlparse(context['uri']).path))
            header = staged_source.parents[1] / '_ludus_graph' / 'graph_resources.luslh'
            self.assertIn('GraphReadOnlyEdgeView Edges;', header.read_text())
            self.assertIn('uint32 LoadWord(in uint32 byteOffset)', header.read_text())
            self.assertNotIn('void StoreWord(in uint32 byteOffset', header.read_text())
            self.assertNotIn('void Store(in uint32 index, in Edge record)', header.read_text())
            luslc = build.language.find_luslc(options['helper'])
            self.check_resource_completion(context, luslc, 'lusl', staged_source.read_text())
            shade_lusl.write_text(lusl_consumer)
            built = build.build_package(manifest_path, output, **options)
            execution.cook_runtime(built['package'], root / 'lusl-named.lurgrt')
            accepted = (output / 'current.json').read_bytes()
            shade_lusl.write_text(lusl_consumer.replace(
                'return edge.EndWorld', 'resources.Edges.Store(0, edge); return edge.EndWorld'))
            with self.assertRaisesRegex(ToolingError, 'Store'):
                build.build_package(manifest_path, output, **options)
            self.assertEqual(accepted, (output / 'current.json').read_bytes())

    def test_mixed_sequence_can_select_triangle_loop_for_one_kernel(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            shutil.copytree(COMPUTE_MIXED, source)
            path = source / 'main.lurender.json'
            document = json.loads(path.read_text())
            node = next(node for node in document['configurations'][0]['nodes'] if node['kind'] == 'ComputePipeline')
            node['invocation']['traversal'] = 'TriangleLoop'
            path.write_bytes(graph.canonical_asset(document))
            manifest = source / 'main.lushader.json'
            manifest.write_text((source / 'main.lushader.json.in').read_text().replace(
                '@RG2_TEST_PROFILES@', '["metal", "wgsl"]'))
            result = build.build_package(manifest, root / 'build', helper=os.environ['LUDUS_RG1_HELPER'],
                                         compiler=os.environ['LUDUS_RG1_SLANG'], spirv_cross=os.environ['LUDUS_RG1_CROSS'])
            package = Path(result['package'])
            descriptor = json.loads((package / 'package.json').read_text())
            recipe = next(recipe for recipe in descriptor['recipes'] if recipe['technique'] == ray.TECHNIQUE)
            self.assertEqual(1, sum(variant[-1] == ray.TRIANGLE_LOOP_TAG for variant in recipe['ray_programs']))
            execution.cook_runtime(str(package), root / 'mixed.lurgrt')

    def test_scene_compute_samples_an_earlier_ray_color_output(self):
        from test_render_graph_authoring import parse_runtime
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package, recipe, wrapper = self.build(SCENE_COMPUTE, root, 'main_shaders/produce_diffraction.slang')
            self.assertTrue(recipe['color_input'])
            self.assertIn('Texture2D<float4> GraphColorInput;', wrapper)
            self.assertIn('RWTexture2D<float4> GraphStorageTexture;', wrapper)
            destination = root / 'color-input.lurgrt'
            execution.cook_runtime(package, destination, configuration='ColorInput')
            _, resources, passes, _ = execution.lower_plan(
                json.loads((package / 'package.json').read_text()), 'ColorInput')
            self.assertEqual(2, len(passes))
            self.assertEqual(passes[0]['Color'], passes[1]['SourceColor'])
            self.assertNotEqual(passes[1]['Color'], passes[1]['SourceColor'])
            self.assertEqual('Color', resources[passes[1]['SourceColor']]['Kind'])
            self.assertEqual((8, 8), (passes[0]['StorageLaunchWidth'], passes[0]['StorageLaunchHeight']))
            self.assertEqual((1, 1), (resources[passes[0]['Color']]['ViewNumerator'],
                                     resources[passes[0]['Color']]['ViewDenominator']))
            parsed = parse_runtime(destination.read_bytes())
            self.assertEqual(execution.PACKAGE_VERSION, parsed['version'])
            self.assertEqual((8, 8, 0, 0), parsed['passes'][0][-4:])

    def test_native_ray_stages_can_write_the_storage_texture(self):
        from test_render_graph_authoring import parse_runtime
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package, recipe, _ = self.build(SCENE_COMPUTE, root, 'main_shaders/read_diffraction.slang',
                                            native_output='storage', lit_consumer=True)
            self.assertTrue(recipe['storage_texture'])
            self.assertIn('native_pipeline', recipe)
            contents = execution._runtime_contents(str(package), 'ColorInput')
            self.assertTrue(contents['ltc_table'])
            native = contents['artifacts'][('NativeRayStages', 'compute', 'metal')]
            self.assertNotIn(ray.LTC_TABLE_SLOT, {entry['binding'] for entry in native['descriptor']['interface']['entries']})
            native_wrapper = (package / 'programs' / recipe['native_pipeline'] / 'wrapper.slang').read_text()
            self.assertNotIn('ReadDiffraction', native_wrapper)
            native_pass = next(item for item in contents['passes'] if item['Traversal'] == 'HardwarePipeline')
            self.assertGreater(native_pass['RayProgram'], 0)
            self.assertRegex(native_wrapper, rf"case {native_pass['RayProgram']}: .*ProduceDiffraction")
            destination = root / 'native-storage.lurgrt'
            execution.cook_runtime(package, destination, configuration='ColorInput')
            stages = parse_runtime(destination.read_bytes())['stages']
            stage = next(stage for stage in stages if stage['role'] == execution.NATIVE_RAY_STAGE_ROLE)
            self.assertEqual((8, 8, 1), stage['workgroup'])

    def test_native_ray_stages_can_return_a_fixed_image_sampled_by_scene_compute(self):
        from test_render_graph_authoring import parse_runtime
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package, recipe, _ = self.build(SCENE_COMPUTE, root, 'main_shaders/produce_diffraction.slang',
                                            native_output='image')
            self.assertIn('native_pipeline', recipe)
            self.assertFalse(recipe.get('storage_texture', False))
            self.assertTrue(recipe['color_input'])
            destination = root / 'native-image.lurgrt'
            execution.cook_runtime(package, destination, configuration='ColorInput')
            parsed = parse_runtime(destination.read_bytes())
            self.assertEqual((0, 0, 8, 8), parsed['passes'][0][-4:])
            self.assertTrue(any(stage['role'] == execution.NATIVE_RAY_STAGE_ROLE for stage in parsed['stages']))

    def test_compute_kernels_dispatch_only_from_ray_generation(self):
        from test_render_graph_authoring import parse_runtime
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # The fixture's Default configuration runs scene_info.slang; WorkItems adds a second ray kernel.
            package, recipe, wrapper = self.build(SCENE_COMPUTE, root, 'main_shaders/scene_info.slang',
                                                 image_launch=[8, 8, 1])
            self.assertIn('case 0: return GraphSource0::ShadeSceneInfo(scene, launch, queries);', wrapper)
            self.assertIn('RWStructuredBuffer<uint4> GraphBuffer0', wrapper)
            self.assertEqual([], self.cases(wrapper, '(scene, hit, queries, payload)'))
            self.assertEqual([], self.cases(wrapper, '(scene, ray, queries, payload)'))
            self.assertEqual([['SceneInfo']], recipe['ray_programs'])
            self.assertTrue(recipe['persistent_buffers'])
            destination = root / 'scene_compute.lurgrt'
            execution.cook_runtime(str(package), destination)
            parsed = parse_runtime(destination.read_bytes())
            self.assertEqual(execution.PACKAGE_VERSION, parsed['version'])
            self.assertEqual((0, 0, 8, 8), parsed['passes'][0][-4:])

            _, _, wrapper = self.build(COMPUTE_MIXED, root, 'main_shaders/primary.slang')
            self.assertIn('case 0: return GraphSource2::TracePrimary(scene, launch, queries);', wrapper)
            self.assertIn('case 1: return GraphSource3::ShadeRecordedHit(scene, launch, queries);', wrapper)
            self.assertEqual(['1'], self.cases(wrapper, '(scene, hit, queries, payload)'))
            self.assertEqual(['1'], self.cases(wrapper, '(scene, ray, queries, payload)'))


if __name__ == '__main__':
    unittest.main()
