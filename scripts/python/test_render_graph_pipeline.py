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
        self.write(node)
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
                self.fails(node, 'IncompletePipeline')
                self.assertEqual(before, self.path.read_bytes())

    def test_compute_and_ray_programs_are_capability_gated(self):
        for kind, stage in (('ComputePipeline', 'compute'), ('RayTracingPipeline', 'ray_generation')):
            node = pipeline.new_node(kind, 'work')
            node['shaders'] = [{'id': 'entry', 'stage': stage, 'program': 'Entry'}]
            self.fails(node, 'Capability')

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


class PipelineBuildTests(unittest.TestCase):
    setUp = fixtures.BuildTests.setUp
    value = fixtures.BuildTests.value
    stage = fixtures.BuildTests.stage
    run_build = fixtures.BuildTests.run_build
    pointer = fixtures.BuildTests.pointer
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
