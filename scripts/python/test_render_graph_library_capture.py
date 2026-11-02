import copy
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_build as build
from ludus_tools import render_graph_library_capture as library_capture
from ludus_tools.errors import ToolingError

ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = ROOT / 'modules/graphics/renderer/render_graphs'


def copy_template(name, destination, profiles='["metal"]'):
    shutil.copytree(TEMPLATES / name, destination)
    manifest = destination / 'main.lushader.json'
    manifest.write_text((destination / 'main.lushader.json.in').read_text().replace(
        '@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@', profiles))
    (destination / 'main.lushader.json.in').unlink()
    return manifest


class CaptureRootTests(unittest.TestCase):
    def setUp(self):
        self.library = json.loads((TEMPLATES / 'salesin_jarosz_lines/library.lurender.json').read_text())

    def test_editor_names_the_same_captured_export(self):
        header = (ROOT / 'apps/editor/src/internal/render_graph_library_capture.h').read_text()
        self.assertIn(f'LIBRARY_CAPTURE_EXPORT = "{library_capture.ROOT_ID}/{library_capture.CAPTURE_OUTPUT}";', header)

    def test_targets_list_each_configuration_color_outputs(self):
        self.assertEqual(library_capture.list_capture_targets(self.library),
                         {'PointLineMis': ['color', 'lineSampling', 'pointSampling']})

    def test_root_routes_a_ray_output_through_accumulate_and_compiles(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'package'
            copy_template('salesin_jarosz_lines', package)
            for output in ('color', 'lineSampling', 'pointSampling'):
                with self.subTest(output=output):
                    root = library_capture.make_capture_root(self.library, 'library.lurender.json', 'PointLineMis', output)
                    path = package / 'capture.lurender.json'
                    path.write_bytes(graph.canonical_asset(root))
                    plan = graph.compile_graph(path, root=package, configuration='Default')
                    kinds = {node['kind'] for node in root['configurations'][0]['nodes']}
                    self.assertTrue({'Accumulate', 'ToneMap', 'Export', 'Subgraph'} <= kinds)
                    edges = root['configurations'][0]['edges']
                    self.assertIn({'from': 'transport.' + output, 'to': 'accumulate.color'}, edges)
                    self.assertTrue(plan['passes'])

    def test_an_already_accumulated_output_goes_straight_to_the_export(self):
        library = copy.deepcopy(self.library)
        record = library['configurations'][0]
        record['nodes'].append({'id': 'resolved', 'kind': 'ToneMap', 'exposure': 1})
        record['outputs'][0]['source'] = 'resolved.color'
        root = library_capture.make_capture_root(library, 'library.lurender.json', 'PointLineMis', 'color')
        kinds = [node['kind'] for node in root['configurations'][0]['nodes']]
        self.assertNotIn('Accumulate', kinds)
        self.assertIn({'from': 'transport.color', 'to': 'output.texture'}, root['configurations'][0]['edges'])

    def test_unknown_names_and_unsupported_interfaces_explain_the_choices(self):
        cases = (
            (self.library, 'Missing', 'color', 'has no configuration Missing. Choose one of: PointLineMis.'),
            (self.library, 'PointLineMis', 'missing',
             'has no output missing. Choose one of: color, lineSampling, pointSampling.'),
        )
        for document, configuration, output, message in cases:
            with self.subTest(configuration=configuration, output=output):
                with self.assertRaises(ToolingError) as caught:
                    library_capture.make_capture_root(document, 'library.lurender.json', configuration, output)
                self.assertEqual(caught.exception.code, 'RenderGraphBuildCapture')
                self.assertIn(message, str(caught.exception))
        depth = copy.deepcopy(self.library)
        depth['configurations'][0]['inputs'].append({'id': 'depth', 'type': 'DepthTexture'})
        with self.assertRaises(ToolingError) as caught:
            library_capture.make_capture_root(depth, 'library.lurender.json', 'PointLineMis', 'color')
        self.assertIn('input depth needs a DepthTexture, but a capture supplies only Scene, View and ColorTexture',
                      str(caught.exception))
        self.assertIn(library_capture.REPAIR, str(caught.exception))


@unittest.skipUnless(all(os.environ.get(name) for name in ('LUDUS_RG1_HELPER', 'LUDUS_RG1_SLANG')),
                     'needs LUDUS_RG1_HELPER and LUDUS_RG1_SLANG')
class LibraryCaptureBuildTests(unittest.TestCase):
    def test_variant_is_cooked_from_the_generation_and_leaves_it_untouched(self):
        from ludus_tools import render_graph_execution as execution
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = copy_template('compute_ray_tracing', root / 'source')
            options = dict(helper=os.environ['LUDUS_RG1_HELPER'], compiler=os.environ['LUDUS_RG1_SLANG'],
                           spirv_cross=os.environ.get('LUDUS_RG1_CROSS'))
            published = build.build_package(manifest, root / 'build', **options)
            execution.cook_runtime(published['package'], root / 'main.lurgrt')
            before = sorted(path.relative_to(root / 'build').as_posix() for path in (root / 'build').rglob('*'))
            # Later edits to the live sources must not reach the variant of the published generation.
            (root / 'source' / 'library.lurender.json').write_text('{}')
            destination = root / 'capture' / 'ComputeRayTracing-color.lurgrt'
            result = library_capture.rebuild_library_capture(
                root / 'build', root / 'capture-build', destination, library='library.lurender.json',
                library_configuration='ComputeRayTracing', library_output='color', **options)
            self.assertEqual(result['source_generation'], published['generation'])
            self.assertEqual(result['export'], library_capture.ROOT_ID + '/' + library_capture.CAPTURE_OUTPUT)
            self.assertIsNone(result['capture_profile'])
            self.assertEqual(destination.read_bytes()[:4], (root / 'main.lurgrt').read_bytes()[:4])
            after = sorted(path.relative_to(root / 'build').as_posix() for path in (root / 'build').rglob('*'))
            self.assertEqual(before, after)
            with self.assertRaises(ToolingError) as caught:
                library_capture.rebuild_library_capture(root / 'build', root / 'capture-build', destination,
                                                library='Missing.lurender.json', library_configuration='X',
                                                library_output='color', **options)
            self.assertIn('insert the library on the root canvas and build the graph again', str(caught.exception))


if __name__ == '__main__':
    unittest.main()
