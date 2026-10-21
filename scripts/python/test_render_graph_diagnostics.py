"""Production validator/CLI batches, exact repair targets and cascade suppression."""
import copy
import io
import json
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
import tempfile
import unittest

from ludus_tools import cli, render_graph as graph
from ludus_tools.errors import ToolingError
from ludus_tools.render_graph_diagnostics import MAX_DIAGNOSTICS


class ActionableDiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = graph.new_workspace(Path(self.temporary.name) / 'graphs', output='offscreen')
        self.document = json.loads(self.path.read_bytes())
        self.config = self.document['configurations'][0]

    def fail(self):
        self.path.write_text(json.dumps(self.document))
        before = self.path.read_bytes()
        with self.assertRaises(ToolingError) as caught:
            graph.compile_graph(self.path)
        self.assertEqual(before, self.path.read_bytes())
        return caught.exception

    def test_independent_wiring_errors_are_ordered_and_execution_cascades_are_suppressed(self):
        for edge in self.config['edges']:
            if edge['to'] in ('raster.scene', 'raster.depth'):
                edge['from'] = 'camera.view'
        error = self.fail()
        self.assertEqual('RenderGraphTypeMismatch', error.code)
        self.assertEqual(2, len(error.diagnostics))
        self.assertEqual(['scene', 'depth'], [issue['location']['port'] for issue in error.diagnostics])
        for issue in error.diagnostics:
            self.assertEqual('raster', issue['location']['node'])
            self.assertEqual('input', issue['location']['direction'])
            self.assertEqual('View', issue['actual'])
            self.assertIn('Connect a', issue['repair'])
            self.assertNotEqual('RenderGraphUnboundInput', issue['code'])

    def test_missing_ports_across_parent_and_nested_subgraph_report_exact_owners(self):
        # Keep the observable raster output; remove bindings in both configurations.
        self.config['edges'] = [edge for edge in self.config['edges'] if edge['to'] != 'raster.scene']
        library = self.path.parent / 'rasterize.lurender.json'
        child = json.loads(library.read_bytes())
        child['configurations'][0]['edges'] = []
        library.write_text(json.dumps(child))
        error = self.fail()
        self.assertGreater(len(error.diagnostics), 1)
        self.assertEqual('scene', error.diagnostics[0]['location']['port'])
        nested = [issue for issue in error.diagnostics if issue['location']['source'] == library.name]
        self.assertEqual({'scene', 'view', 'color', 'depth'}, {issue['location']['port'] for issue in nested})
        self.assertTrue(all(issue['location']['instance'] == 'Main/raster/draw' for issue in nested))

    def test_batch_is_bounded_with_an_explicit_omission_notice(self):
        self.config['nodes'] = [{'id': f'export{i}', 'kind': 'Export', 'type': 'ColorTexture'}
                                for i in range(MAX_DIAGNOSTICS + 5)]
        self.config['edges'] = []
        error = self.fail()
        self.assertEqual(MAX_DIAGNOSTICS, len(error.diagnostics))
        self.assertEqual('RenderGraphDiagnosticLimit', error.diagnostics[-1]['code'])
        self.assertIn('validate again', error.diagnostics[-1]['message'])
        self.assertLess(len(json.dumps(error.diagnostics).encode()), 256 * 1024)

    def test_structural_errors_in_independent_configurations_are_collected(self):
        second = copy.deepcopy(self.config)
        second['id'] = 'Other'
        self.document['configurations'].append(second)
        for config in self.document['configurations']:
            config['edges'][0]['from'] = 'missing.texture'
        error = self.fail()
        self.assertEqual(['Default', 'Other'], [issue['location']['configuration'] for issue in error.diagnostics])
        self.assertTrue(all(issue['code'] == 'RenderGraphPort' for issue in error.diagnostics))

    def test_json_cli_exposes_the_same_batch_and_stable_first_error(self):
        self.config['edges'] = []
        error = self.fail()
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertNotEqual(0, cli.main(['--json', 'render-graph', 'validate', str(self.path)]))
        response = json.loads(output.getvalue())['error']
        self.assertEqual(error.code, response['code'])
        self.assertEqual(error.diagnostics, response['diagnostics'])
        self.assertTrue(all('expected' in issue and 'actual' in issue and 'repair' in issue
                            for issue in response['diagnostics']))

    def test_text_cli_prints_every_issue_and_repair_without_a_duplicate_summary(self):
        self.config['edges'] = []
        error = self.fail()
        output = io.StringIO()
        with redirect_stderr(output):
            self.assertNotEqual(0, cli.main(['render-graph', 'validate', str(self.path)]))
        text = output.getvalue()
        self.assertEqual(len(error.diagnostics), sum(line.startswith('error [') for line in text.splitlines()))
        self.assertIn('Expected:', text)
        self.assertIn('Actual:', text)
        self.assertIn('Repair:', text)


if __name__ == '__main__':
    unittest.main()
