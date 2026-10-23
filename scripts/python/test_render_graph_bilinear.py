"""Executable bilinear sizing, resources, diagnostics and runtime codecs."""
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from ludus_tools import render_graph as graph, render_graph_execution as execution
from ludus_tools.errors import ToolingError
import test_render_graph_execution as fixtures
import test_render_graph_authoring as codec


def insert_bilinear(path):
    document = json.loads(path.read_text())
    document['version'] = 2
    configuration = document['configurations'][0]
    color = next(node for node in configuration['nodes'] if node['id'] == 'color')
    destination = copy.deepcopy(color)
    destination['id'] = 'destination'
    color.update(imported=False, presentation=False, extent='RenderSize')
    next(node for node in configuration['nodes'] if node['id'] == 'depth')['extent'] = 'RenderSize'
    destination['extent'] = 'RenderSize'
    configuration['nodes'].extend([
        {'id': 'outputSize', 'kind': 'OutputSize', 'extent': [9, 7]},
        {'id': 'renderSize', 'kind': 'RenderSize', 'extent': 'OutputSize', 'scale': [1, 2]},
        destination, {'id': 'upscale', 'kind': 'Upscale', 'algorithm': 'bilinear'}])
    for edge in configuration['edges']:
        if edge['to'] == 'camera.target':
            edge['from'] = 'destination.texture'
        if edge['to'] in ('output.color', 'output.texture'):
            edge['from'] = 'upscale.color'
    configuration['edges'].extend([
        {'from': 'outputSize.size', 'to': 'renderSize.output'},
        {'from': 'renderSize.size', 'to': 'color.size'},
        {'from': 'renderSize.size', 'to': 'depth.size'},
        {'from': 'outputSize.size', 'to': 'destination.size'},
        {'from': 'raster.color', 'to': 'upscale.color'},
        {'from': 'destination.texture', 'to': 'upscale.destination'}])
    path.write_bytes(graph.canonical_asset(document))
    return document


class BilinearTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = graph.new_workspace(Path(temporary.name) / 'graph', output='offscreen')
        self.document = insert_bilinear(self.path)

    def test_odd_extents_and_camera_route_use_distinct_source_and_destination(self):
        plan = graph.compile_graph(self.path)
        self.assertEqual(['Raster', 'Upscale', 'Export'], [p['kind'] for p in plan['passes']])
        self.assertEqual([5, 4], plan['resources']['Main/color']['extent'])
        self.assertEqual([9, 7], plan['resources']['Main/destination']['extent'])
        self.assertEqual('Main/destination', plan['roots'][0]['resource'])
        self.assertEqual(['Main/camera'], plan['roots'][0]['cameras'])
        self.assertEqual('Main/color', plan['passes'][1]['source_color']['resource'])

    def test_invalid_alias_undefined_input_and_multisample_reject(self):
        for mutation in ('alias', 'undefined', 'samples'):
            document = copy.deepcopy(self.document)
            configuration = document['configurations'][0]
            if mutation == 'samples':
                next(node for node in configuration['nodes'] if node['id'] == 'destination')['samples'] = 4
            else:
                edge = next(edge for edge in configuration['edges'] if edge['to'] == 'upscale.' + ('destination' if mutation == 'alias' else 'color'))
                edge['from'] = 'raster.color' if mutation == 'alias' else 'color.texture'
            self.path.write_text(json.dumps(document))
            with self.subTest(mutation=mutation), self.assertRaises(ToolingError):
                graph.compile_graph(self.path)

    def test_viewport_policy_preserves_distinct_render_scale(self):
        configuration = self.document['configurations'][0]
        next(node for node in configuration['nodes'] if node['id'] == 'outputSize')['extent'] = 'Viewport'
        self.path.write_bytes(graph.canonical_asset(self.document))
        plan = graph.compile_graph(self.path)
        self.assertEqual([1, 2], plan['resources']['Main/color']['view_scale'])
        self.assertEqual([1, 1], plan['resources']['Main/destination']['view_scale'])


class BilinearExecutionTests(unittest.TestCase):
    setUp = fixtures.ExecutionTests.setUp
    stage = fixtures.ExecutionTests.stage
    value = fixtures.ExecutionTests.value
    edit = fixtures.ExecutionTests.edit
    run_build = fixtures.ExecutionTests.run_build

    def test_bilinear_serializes_as_an_image_pass_in_both_emitters(self):
        insert_bilinear(self.graph)
        package = Path(self.run_build()['package'])
        contents = execution._runtime_contents(package, 'Default')
        passes = contents['passes']
        self.assertEqual(['ColorAndDepth', 'BilinearUpscale'], [p['Program'] for p in passes])
        self.assertEqual(0, passes[1]['SceneData'])
        self.assertNotEqual(passes[1]['SourceColor'], passes[1]['Color'])
        runtime = execution.encode_runtime_package(contents)
        parsed = codec.parse_runtime(runtime)
        self.assertEqual(2, len(parsed['passes']))
        self.assertEqual(execution.PROGRAM_CODES['BilinearUpscale'], parsed['passes'][1][0])
        self.assertEqual(passes[1]['SourceColor'], parsed['passes'][1][-1])
        self.assertEqual(['Main/raster/draw', 'Main/upscale'], parsed['pass_names'])
        header_path = self.root / 'bilinear.h'
        execution.emit_runtime(package, header_path, configuration='Default')
        header = header_path.read_text()
        self.assertIn('Main/upscale', header)
