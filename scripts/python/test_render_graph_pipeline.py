"""Pipeline drafts, migration, admission and failure-preserving package builds."""
import copy
import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import tempfile
import unittest

from ludus_tools import cli, render_graph as graph, render_graph_build as build
from ludus_tools import render_graph_pipeline as pipeline
from ludus_tools.errors import ToolingError
import test_render_graph_build as fixtures


class PipelineDocumentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.path = self.root / 'main.lurender.json'

    def write(self, node):
        value = {'version': 2, 'id': 'Pipelines', 'default': 'Default', 'configurations': [
            {'id': 'Default', 'inputs': [], 'outputs': [], 'nodes': [node], 'edges': []}]}
        self.path.write_bytes(graph.canonical_asset(value))
        return value

    def fails(self, node, code):
        node['inputs'] = [{'id': 'color', 'type': 'ColorTexture', 'required': True}]
        node['outputs'] = [{'id': 'color', 'type': 'ColorTexture', 'target': 'color',
                            'format': 'RGBA8Unorm', 'load': 'Clear', 'store': 'Store'}]
        value = self.write(node)
        configuration = value['configurations'][0]
        configuration['nodes'].extend([graph._target('target', 'ColorTexture'),
                                       {'id': 'output', 'kind': 'Export', 'type': 'ColorTexture'}])
        configuration['edges'] = [{'from': 'target.texture', 'to': 'work.color'},
                                  {'from': 'work.color', 'to': 'output.texture'}]
        self.path.write_bytes(graph.canonical_asset(value))
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(self.path)
        self.assertEqual('RenderGraph' + code, failure.exception.code)
        self.assertEqual('work', failure.exception.location['node'])

    def test_three_empty_drafts_save_and_inspect_but_do_not_execute(self):
        for kind in pipeline.KINDS:
            with self.subTest(kind=kind):
                node = pipeline.new_node(kind, 'work')
                self.write(node)
                before = self.path.read_bytes()
                self.assertEqual('InterfacesOnly', graph.load_library(self.path)['validation'])
                with self.assertRaises(ToolingError) as failure:
                    graph.compile_graph(self.path)
                self.assertEqual('RenderGraphUnboundOutput', failure.exception.code)
                self.assertEqual(before, self.path.read_bytes())

    def test_generic_compute_is_capability_gated_and_scene_compute_lowers(self):
        for kind, stage in (('ComputePipeline', 'compute'), ('RayTracingPipeline', 'ray_generation')):
            node = pipeline.new_node(kind, 'work')
            node['shaders'] = [{'id': 'entry', 'stage': stage, 'program': 'Entry'}]
            self.fails(node, 'Capability' if kind == 'ComputePipeline' else 'IncompletePipeline')
        # A Scene input makes the same compute node a member of the ordered ray sequence.
        node = pipeline.new_node('ComputePipeline', 'work')
        node.update(state={}, invocation={'dimensions': 'ColorTarget'},
                    shaders=[{'id': 'entry', 'stage': 'compute', 'program': 'Entry'}],
                    inputs=[{'id': 'scene', 'type': 'Scene', 'required': True},
                            {'id': 'view', 'type': 'View', 'required': True},
                            {'id': 'color', 'type': 'ColorTexture', 'required': True}],
                    outputs=[{'id': 'color', 'type': 'ColorTexture', 'target': 'color',
                              'format': 'RGBA8Unorm', 'load': 'Clear', 'store': 'Store'}])
        self.assertTrue(pipeline.is_scene_compute(node))
        value = self.write(node)
        value['configurations'][0]['nodes'].extend([
            {'id': 'scene', 'kind': 'Scene'}, graph._target('target', 'ColorTexture'),
            {'id': 'camera', 'kind': 'Camera', 'active': True, 'role': 'Offscreen', 'target_type': 'ColorTexture'},
            {'id': 'output', 'kind': 'Export', 'type': 'ColorTexture'}])
        value['configurations'][0]['edges'] = [
            {'from': 'scene.scene', 'to': 'work.scene'}, {'from': 'camera.view', 'to': 'work.view'},
            {'from': 'target.texture', 'to': 'work.color'}, {'from': 'target.texture', 'to': 'camera.target'},
            {'from': 'work.color', 'to': 'output.texture'}]
        self.path.write_bytes(graph.canonical_asset(value))
        work = next(operation for operation in graph.compile_graph(self.path)['passes'] if operation['id'].endswith('/work'))
        self.assertEqual(('RayTracing', 'ComputePipeline'), (work['kind'], work['source_kind']))
        # Generic drafts keep their authored shape byte for byte.
        draft = pipeline.new_node('ComputePipeline', 'work')
        self.write(draft)
        before = self.path.read_bytes()
        self.assertEqual('InterfacesOnly', graph.load_library(self.path)['validation'])
        self.assertEqual(before, self.path.read_bytes())
        self.assertEqual({'group_size': [1, 1, 1]}, draft['state'])

    def test_stage_legality_duplicate_ids_dimensions_and_unknown_state(self):
        node = pipeline.new_node('ComputePipeline', 'work')
        original = copy.deepcopy(node)
        changes = [lambda n: n['shaders'].append({'id': 'shader', 'stage': 'vertex', 'program': 'Vertex'}),
                   lambda n: n['invocation'].update(dimensions=[0, 1, 1]),
                   lambda n: n['state'].update(hidden_option=True),
                   lambda n: n['inputs'].extend([{'id': 'data', 'type': 'Buffer', 'required': True}] * 2)]
        for change in changes:
            node = copy.deepcopy(original)
            change(node)
            with self.assertRaises(ToolingError):
                self.write(node)
        self.assertEqual(original, pipeline.new_node('ComputePipeline', 'work'))

    def test_custom_ports_are_checked_for_type_and_multiple_writers(self):
        node = pipeline.new_node('ComputePipeline', 'work')
        node['inputs'] = [{'id': 'image', 'type': 'ColorTexture', 'required': True}]
        value = self.write(node)
        configuration = value['configurations'][0]
        configuration['nodes'].append(graph._target('target', 'ColorTexture'))
        configuration['edges'] = [{'from': 'target.texture', 'to': 'work.image'}] * 2
        self.path.write_text(json.dumps(value))
        with self.assertRaises(ToolingError) as failure:
            graph.load_library(self.path)
        self.assertEqual('RenderGraphMultipleWriters', failure.exception.code)
        configuration['edges'] = [{'from': 'target.texture', 'to': 'work.missing'}]
        self.path.write_text(json.dumps(value))
        with self.assertRaises(ToolingError) as failure:
            graph.load_library(self.path)
        self.assertEqual('RenderGraphPort', failure.exception.code)
        node['outputs'] = [{'id': 'result', 'type': 'DepthTexture', 'target': 'image',
                            'format': 'Depth32Float', 'load': 'Clear', 'store': 'Store'}]
        with self.assertRaises(ToolingError) as failure:
            self.write(node)
        self.assertEqual('RenderGraphTypeMismatch', failure.exception.code)

    def test_output_pair_diagnostics_name_the_resource_and_repair(self):
        node = pipeline.new_node('ComputePipeline', 'work')
        node['inputs'] = [{'id': 'color', 'type': 'ColorTexture', 'required': True}]
        node['outputs'] = [{'id': 'color', 'type': 'ColorTexture', 'target': 'input',
                            'format': 'RGBA8Unorm', 'load': 'Clear', 'store': 'Store'}]
        for target, kind, expected in [('input', 'ColorTexture', "input 'input' does not exist"),
                                        ('color', 'DepthTexture', "input 'color' has type ColorTexture")]:
            node['outputs'][0].update(target=target, type=kind)
            with self.subTest(target=target, kind=kind), self.assertRaises(ToolingError) as failure:
                self.write(node)
            self.assertEqual('RenderGraphTypeMismatch', failure.exception.code)
            self.assertIn("Output 'color'", str(failure.exception))
            self.assertIn(expected, str(failure.exception))
            self.assertIn('Pair it with' if target == 'input' else 'Add a writable', str(failure.exception))
        node['outputs'][0].update(target='color', type='ColorTexture', format='Depth32Float')
        with self.assertRaises(ToolingError) as failure:
            self.write(node)
        self.assertIn("Output 'color'", str(failure.exception))
        self.assertIn('Set Format to RGBA8Unorm', str(failure.exception))

    def test_writable_input_must_be_connected_even_if_optional_and_cleared(self):
        node = pipeline.new_node('ComputePipeline', 'work')
        node['inputs'] = [{'id': 'color', 'type': 'ColorTexture', 'required': False}]
        node['outputs'] = [{'id': 'result', 'type': 'ColorTexture', 'target': 'color',
                            'format': 'RGBA8Unorm', 'load': 'Clear', 'store': 'Store'}]
        value = self.write(node)
        configuration = value['configurations'][0]
        configuration['nodes'].append({'id': 'output', 'kind': 'Export', 'type': 'ColorTexture'})
        configuration['edges'] = [{'from': 'work.result', 'to': 'output.texture'}]
        self.path.write_bytes(graph.canonical_asset(value))
        before = self.path.read_bytes()
        self.assertEqual('InterfacesOnly', graph.load_library(self.path)['validation'])
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(self.path)
        self.assertEqual('RenderGraphUnboundInput', failure.exception.code)
        self.assertIn("'work.color' (ColorTexture)", str(failure.exception))
        self.assertIn('Connect a matching output', str(failure.exception))
        self.assertEqual(before, self.path.read_bytes())

    def test_duplicate_resource_writes_and_field_errors_include_repairs(self):
        node = pipeline.new_node('ComputePipeline', 'work')
        node['inputs'] = [{'id': 'color', 'type': 'ColorTexture', 'required': True}]
        node['outputs'] = [{'id': name, 'type': 'ColorTexture', 'target': 'color',
                            'format': 'RGBA8Unorm', 'load': 'Clear', 'store': 'Store'} for name in ('first', 'second')]
        with self.assertRaises(ToolingError) as failure:
            self.write(node)
        self.assertEqual('RenderGraphMultipleWriters', failure.exception.code)
        self.assertIn("Outputs 'first' and 'second'", str(failure.exception))
        self.assertIn('separate input', str(failure.exception))
        node['outputs'] = []
        node['state']['group_size'] = [1, 0, 1]
        with self.assertRaises(ToolingError) as failure:
            self.write(node)
        self.assertIn('group_size.y', str(failure.exception))
        self.assertIn('Fix Pipeline State', str(failure.exception))


class PipelineBuildTests(unittest.TestCase):
    setUp = fixtures.BuildTests.setUp
    value = fixtures.BuildTests.value
    stage = fixtures.BuildTests.stage
    run_build = fixtures.BuildTests.run_build
    pointer = fixtures.BuildTests.pointer

    def test_unused_draft_does_not_require_a_manifest_program_or_compile_it(self):
        value = json.loads(self.graph.read_text())
        value['version'] = 2
        draft = pipeline.new_node('ComputePipeline', 'draft')
        draft['shaders'] = [{'id': 'compute', 'stage': 'compute', 'program': 'UnregisteredDraft'}]
        draft['inputs'] = [{'id': 'unbound', 'type': 'Buffer', 'required': True}]
        value['configurations'][0]['nodes'].append(draft)
        self.graph.write_bytes(graph.canonical_asset(value))
        before = self.graph.read_bytes()
        result = self.run_build()
        self.assertEqual(6, result['cache_misses'])
        self.assertEqual(before, self.graph.read_bytes())

    def migrate(self):
        path = self.graph.parent / 'rasterize.lurender.json'
        original = json.loads(path.read_text())
        migrated = pipeline.migrate_document(original, self.value())
        path.write_bytes(graph.canonical_asset(migrated))
        return path, original, migrated

    def test_migration_preserves_resource_versions_routes_and_programs(self):
        original_plan = graph.compile_graph(self.graph)
        path, original, migrated = self.migrate()
        changed_plan = graph.compile_graph(self.graph)
        for entry in changed_plan['passes']:
            entry.pop('pipeline', None)
        for key in ('passes', 'resources', 'roots', 'cameras', 'techniques'):
            self.assertEqual(original_plan[key], changed_plan[key])
        self.assertEqual(1, original['version'])
        self.assertEqual(2, migrated['version'])
        result = self.run_build()
        self.assertEqual(6, result['cache_misses'])
        self.assertEqual(migrated, json.loads(path.read_text()))

    def test_exposed_program_choice_uses_explicit_recipe_and_preserves_prior_package_on_rejection(self):
        path, _, migrated = self.migrate()
        configuration = migrated['configurations'][0]
        node = configuration['nodes'][0]
        declaration = {'id': 'Quality', 'label': 'Quality', 'type': 'Enum',
                       'target': 'shaders.fragment.program', 'values': ['Color', 'Alternate'], 'default': 'Color'}
        node['options'] = [declaration]
        configuration['options'] = [{**declaration, 'target': 'draw.Quality'}]
        path.write_bytes(graph.canonical_asset(migrated))
        manifest = self.value()
        alternate = {**next(program for program in manifest['programs'] if program['id'] == 'Color'), 'id': 'Alternate'}
        manifest['programs'].append(alternate)
        technique = next(item for item in manifest['techniques'] if item['id'].endswith('.ColorAndDepth'))
        manifest['techniques'].append({**copy.deepcopy(technique), 'id': technique['id'] + '.Geometry.Alternate', 'fragment': 'Alternate'})
        self.manifest.write_text(json.dumps(manifest))
        root = json.loads(self.graph.read_text())
        reference = next(item for item in root['configurations'][0]['nodes'] if item['kind'] == 'Subgraph')
        reference['overrides'] = {'Quality': 'Alternate'}
        self.graph.write_bytes(graph.canonical_asset(root))
        result = self.run_build()
        package = build.verify_package(Path(result['package']))
        operation = next(item for item in package['plans'][0]['passes'] if 'pipeline' in item)
        self.assertTrue(operation['technique'].endswith('.Geometry.Alternate'))
        self.assertEqual('Alternate', operation['pipeline']['shaders'][1]['program'])
        pointer = self.pointer()
        reference['overrides'] = {'Quality': 'Hidden'}
        self.graph.write_bytes(graph.canonical_asset(root))
        with self.assertRaises(ToolingError) as failure:
            self.run_build()
        self.assertEqual('RenderGraphOption', failure.exception.code)
        self.assertEqual(pointer, self.pointer())

    def test_mismatched_node_shader_or_state_preserves_published_generation(self):
        self.run_build()
        pointer = self.pointer()
        path, _, migrated = self.migrate()
        for field in ('shaders', 'state'):
            candidate = copy.deepcopy(migrated)
            node = candidate['configurations'][0]['nodes'][0]
            if field == 'shaders':
                node['shaders'][0]['program'] = 'OtherVertex'
            else:
                node['state']['blend'] = 'Alpha'
            path.write_bytes(graph.canonical_asset(candidate))
            with self.assertRaises(ToolingError):
                self.run_build()
            self.assertEqual(pointer, self.pointer())

    def test_custom_graphics_port_names_lower_without_changing_execution(self):
        path, _, migrated = self.migrate()
        configuration = migrated['configurations'][0]
        node = configuration['nodes'][0]
        node['inputs'][0]['id'] = 'world'
        node['invocation']['scene'] = 'world'
        node['outputs'][1]['id'] = 'beauty'
        for edge in configuration['edges']:
            if edge['to'] == 'draw.scene':
                edge['to'] = 'draw.world'
        for output in configuration['outputs']:
            if output['source'] == 'draw.color':
                output['source'] = 'draw.beauty'
        path.write_bytes(graph.canonical_asset(migrated))
        self.assertEqual('Main/scene', graph.compile_graph(self.graph)['passes'][0]['scene'])
        self.run_build()

    def test_cli_conversion_is_explicit_and_read_only(self):
        path = self.graph.parent / 'rasterize.lurender.json'
        before = path.read_bytes()
        output = io.StringIO()
        with redirect_stdout(output):
            status = cli.main(['--json', 'render-graph', 'migrate-pipelines', str(path), '--manifest', str(self.manifest)])
        self.assertEqual(0, status)
        self.assertEqual(2, json.loads(output.getvalue())['document']['version'])
        self.assertEqual(before, path.read_bytes())

    def test_ambiguous_migration_preserves_input(self):
        value = self.value()
        value['techniques'][0]['recipes'].append({**value['techniques'][0]['recipes'][0], 'id': 'Other'})
        original = json.loads((self.graph.parent / 'rasterize.lurender.json').read_text())
        before = copy.deepcopy(original)
        with self.assertRaises(ToolingError):
            pipeline.migrate_document(original, value)
        self.assertEqual(before, original)


class PipelineExecutionTests(unittest.TestCase):
    def test_migrated_static_node_emits_the_identical_numeric_execution_contract(self):
        import test_render_graph_execution as execution_fixture
        from ludus_tools import render_graph_execution as execution
        self.stage = lambda command, cancel: execution_fixture.ExecutionTests.stage(self, command, cancel)
        self.value = lambda: fixtures.BuildTests.value(self)
        execution_fixture.ExecutionTests.setUp(self)
        original = build.verify_package(Path(fixtures.BuildTests.run_build(self)['package']))
        before = execution.lower_plan(original, 'Default')[1:]
        path = self.graph.parent / 'rasterize.lurender.json'
        migrated = pipeline.migrate_document(json.loads(path.read_text()), self.value())
        path.write_bytes(graph.canonical_asset(migrated))
        changed = build.verify_package(Path(fixtures.BuildTests.run_build(self)['package']))
        self.assertEqual(before, execution.lower_plan(changed, 'Default')[1:])
        self.assertEqual(original['counts']['unique_stage_programs'], changed['counts']['unique_stage_programs'])
        self.assertEqual(original['counts']['unique_pipeline_recipes'], changed['counts']['unique_pipeline_recipes'])
