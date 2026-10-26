"""MetalFX Spatial authored color interpretation and versioned image roles."""
import copy
import json
from pathlib import Path
import unittest
from ludus_tools import render_graph as graph, render_graph_execution as execution
from ludus_tools.errors import ToolingError
import test_render_graph_bilinear as bilinear
import test_render_graph_execution as fixtures
import test_render_graph_authoring as codec


class MetalFxTests(bilinear.BilinearTests):
    def setUp(self):
        super().setUp()
        self.node = next(node for node in self.document['configurations'][0]['nodes'] if node['id'] == 'upscale')
        self.node['algorithm'] = 'metalfx-spatial'
        self.path.write_bytes(graph.canonical_asset(self.document))

    def test_color_processing_defaults_and_validates_explicit_modes(self):
        self.assertEqual('Linear', graph.compile_graph(self.path)['passes'][1]['color_processing'])
        self.node['color_processing'] = 'Perceptual'
        self.path.write_bytes(graph.canonical_asset(self.document))
        self.assertEqual('Perceptual', graph.compile_graph(self.path)['passes'][1]['color_processing'])
        self.node['color_processing'] = 'HDR'
        self.path.write_text(json.dumps(self.document))
        with self.assertRaises(ToolingError):
            graph.compile_graph(self.path)

    def test_downscaling_and_other_algorithm_color_options_reject(self):
        for mutation in ('downscale', 'other_algorithm'):
            document = copy.deepcopy(self.document)
            nodes = document['configurations'][0]['nodes']
            if mutation == 'downscale':
                next(node for node in nodes if node['id'] == 'renderSize')['scale'] = [2, 1]
            else:
                next(node for node in nodes if node['id'] == 'upscale').update(algorithm='bilinear', color_processing='Linear')
            self.path.write_text(json.dumps(document))
            with self.subTest(mutation=mutation), self.assertRaises(ToolingError):
                graph.compile_graph(self.path)


class MetalFxExecutionTests(unittest.TestCase):
    setUp = fixtures.ExecutionTests.setUp
    stage = fixtures.ExecutionTests.stage
    value = fixtures.ExecutionTests.value
    edit = fixtures.ExecutionTests.edit
    run_build = fixtures.ExecutionTests.run_build

    def test_both_color_modes_lower_to_native_image_roles(self):
        document = bilinear.insert_bilinear(self.graph)
        node = next(node for node in document['configurations'][0]['nodes'] if node['id'] == 'upscale')
        for mode, program in (('Linear', 'MetalFxSpatialUpscale'), ('Perceptual', 'MetalFxSpatialPerceptualUpscale')):
            node.update(algorithm='metalfx-spatial', color_processing=mode)
            self.graph.write_bytes(graph.canonical_asset(document))
            package = Path(self.run_build()['package'])
            contents = execution._runtime_contents(package, 'Default')
            self.assertEqual(program, contents['passes'][1]['Program'])
            parsed = codec.parse_runtime(execution.encode_runtime_package(contents))
            self.assertEqual(execution.PACKAGE_VERSION, parsed['version'])
            self.assertEqual(execution.PROGRAM_CODES[program], parsed['passes'][1][0])
            self.assertEqual(contents['passes'][1]['SourceColor'], parsed['passes'][1][-2])
            self.assertEqual(0, parsed['passes'][1][-1])
            header = self.root / 'metalfx.h'
            execution.emit_runtime(package, header, configuration='Default')
            self.assertIn('static_cast<format::Technique>(' + str(execution.PROGRAM_CODES[program]) + ')', header.read_text())
