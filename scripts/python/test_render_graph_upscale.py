"""Upscaling drafts, typed composition and explicit unavailable execution."""
import json
from pathlib import Path
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_upscale as upscale
from ludus_tools.errors import ToolingError
import test_render_graph as fixtures


class UpscaleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'main.lurender.json'

    def document(self, algorithm):
        return {'version': 2, 'id': 'Upscaling', 'default': 'Default', 'configurations': [
            {'id': 'Default', 'inputs': [], 'outputs': [], 'nodes': [upscale.new_node(algorithm, 'upscale')],
             'edges': []}]}

    def write(self, value):
        self.path.write_bytes(graph.canonical_asset(value))

    def test_every_preset_saves_and_inspects_without_an_sdk(self):
        for algorithm in upscale.ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                self.write(self.document(algorithm))
                before = self.path.read_bytes()
                self.assertEqual('InterfacesOnly', graph.load_library(self.path)['validation'])
                self.assertEqual(before, self.path.read_bytes())

    def test_version_unknown_algorithm_unknown_fields_and_reserved_identity_reject(self):
        for change in (lambda d: d.update(version=1),
                       lambda d: d['configurations'][0]['nodes'][0].update(algorithm='automatic'),
                       lambda d: d['configurations'][0]['nodes'][0].update(shaders=[]),
                       lambda d: d['configurations'][0]['nodes'][0].update(id='input')):
            value = self.document('fsr-1')
            change(value)
            self.path.write_text(json.dumps(value))
            with self.assertRaises(ToolingError):
                graph.load_library(self.path)

    def test_selected_presets_report_node_availability_even_when_inputs_are_missing(self):
        for algorithm in upscale.ALGORITHMS:
            if algorithm == 'bilinear':
                continue
            with self.subTest(algorithm=algorithm):
                value = self.document(algorithm)
                configuration = value['configurations'][0]
                configuration['nodes'].append({'id': 'output', 'kind': 'Export', 'type': 'ColorTexture'})
                configuration['edges'].append({'from': 'upscale.color', 'to': 'output.texture'})
                self.write(value)
                before = self.path.read_bytes()
                with self.assertRaises(ToolingError) as failure:
                    graph.compile_graph(self.path)
                issues = failure.exception.issues
                capability = next(issue for issue in issues if issue.code == 'RenderGraphCapability')
                self.assertEqual('algorithm', capability.target['field'])
                self.assertEqual('Upscaling/upscale', capability.target['instance'])
                self.assertEqual(algorithm, capability.details['actual'])
                self.assertIn('authoring preset only', capability.message)
                self.assertIn('reconnect', capability.details['repair'])
                self.assertEqual(before, self.path.read_bytes())

    def test_complete_spatial_route_still_rejects_instead_of_copying_or_dispatching(self):
        value = self.document('metalfx-spatial')
        configuration = value['configurations'][0]
        for identity in ('source', 'destination'):
            target = graph._target(identity, 'ColorTexture')
            target.update(imported=True, initialized=identity == 'source')
            configuration['nodes'].append(target)
        configuration['nodes'].append({'id': 'output', 'kind': 'Export', 'type': 'ColorTexture'})
        configuration['edges'] = [{'from': 'source.texture', 'to': 'upscale.color'},
                                   {'from': 'destination.texture', 'to': 'upscale.destination'},
                                   {'from': 'upscale.color', 'to': 'output.texture'}]
        self.write(value)
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(self.path)
        self.assertEqual('RenderGraphCapability', failure.exception.code)
        self.assertEqual('upscale', failure.exception.location['node'])

    def test_wrong_port_types_and_removed_temporal_ports_reject_even_in_unused_drafts(self):
        for algorithm, port in (('fsr-2', 'depth'), ('fsr-1', 'motion')):
            value = self.document(algorithm)
            configuration = value['configurations'][0]
            configuration['nodes'].append(graph._target('source', 'ColorTexture'))
            configuration['edges'].append({'from': 'source.texture', 'to': 'upscale.' + port})
            self.write(value)
            with self.assertRaises(ToolingError) as failure:
                graph.load_library(self.path)
            self.assertEqual('RenderGraphTypeMismatch' if algorithm == 'fsr-2' else 'RenderGraphPort',
                             failure.exception.code)


class UpscaleSelectionTests(unittest.TestCase):
    setUp = fixtures.RenderGraphTests.setUp
    read = fixtures.RenderGraphTests.read
    write = fixtures.RenderGraphTests.write
    config = fixtures.RenderGraphTests.config

    def test_unused_presets_preserve_existing_plan_and_sources(self):
        before = graph.compile_graph(self.path)
        value = self.read()
        value['version'] = 2
        self.config(value)['nodes'].extend(upscale.new_node(algorithm, algorithm) for algorithm in upscale.ALGORITHMS)
        self.write(value)
        sources = {path.name: path.read_bytes() for path in self.directory.iterdir()}
        after = graph.compile_graph(self.path)
        for key in ('passes', 'resources', 'roots', 'cameras', 'techniques'):
            self.assertEqual(before[key], after[key])
        self.assertEqual(sources, {path.name: path.read_bytes() for path in self.directory.iterdir()})

    def test_nested_unused_output_is_culled_and_selected_output_is_gated(self):
        child = self.read('rasterize.lurender.json')
        child['version'] = 2
        self.config(child)['nodes'].append(upscale.new_node('dlss-sr', 'upscale'))
        self.config(child)['outputs'].append({'id': 'upscaled', 'type': 'ColorTexture',
                                              'source': 'upscale.color', 'required': False})
        self.write(child, 'rasterize.lurender.json')
        self.assertEqual(2, len(graph.compile_graph(self.path)['passes']))
        value = self.read()
        next(edge for edge in self.config(value)['edges'] if edge['to'] == 'output.color')['from'] = 'raster.upscaled'
        self.write(value)
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(self.path)
        capability = next(issue for issue in failure.exception.issues if issue.code == 'RenderGraphCapability')
        self.assertEqual('Main/raster/upscale', capability.target['instance'])
        self.assertIn('rasterize.lurender.json', capability.where)


if __name__ == '__main__':
    unittest.main()
