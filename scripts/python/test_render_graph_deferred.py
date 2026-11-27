"""Deferred callback selection and stable cooked-role ABI regressions."""
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import test_render_graph_build as fixtures
import test_render_graph_ray as ray_tests
from ludus_tools import render_graph as graph
from ludus_tools import render_graph_build as build
from ludus_tools import render_graph_execution as execution
from ludus_tools import render_graph_forward as forward


class DeferredTests(unittest.TestCase):
    def test_deferred_lighting_callback_survives_draft_program_pruning(self):
        fixture = fixtures.BuildTests()
        fixture.setUp()
        try:
            fixture.graph = graph.new_workspace(fixture.root / 'deferred', output='offscreen',
                                                renderer_static=True, profiles=('metal', 'wgsl'))
            fixture.manifest = fixture.graph.with_name('main.lushader.json')
            value = json.loads(fixture.manifest.read_text())
            draft = dict(value['programs'][1], id='UnusedDraft', function='DraftMain')
            value['programs'].append(draft)
            fixture.manifest.write_text(json.dumps(value))

            class ReachedExpansion(Exception):
                pass

            def inspect(programs, techniques, *_, **__):
                self.assertIn('DeferredLighting', programs)
                self.assertNotIn('UnusedDraft', programs)
                self.assertTrue(forward.is_deferred(techniques['DefaultStaticOpaque.ColorAndDepth']))
                raise ReachedExpansion

            with patch.object(forward, 'expand', side_effect=inspect), self.assertRaises(ReachedExpansion):
                fixture.run_build()
        finally:
            fixture.tearDown()

    def test_deferred_roles_append_after_clustered_depth_without_renumbering(self):
        first = execution.FORWARD_DEPTH_STAGE_ROLE + 1
        # Version 31 appends the opaque G-buffer role after the original four.
        expected = [('DeferredGBuffer', 'fragment'), ('DeferredLighting', 'vertex'),
                    ('DeferredLighting', 'fragment'), ('DeferredDepthCopy', 'fragment'),
                    ('DeferredOpaqueGBuffer', 'fragment')]
        self.assertEqual([first + index for index in range(len(expected))],
                         [execution.role(*key) for key in expected])
        self.assertEqual(execution.FORWARD_DEPTH_STAGE_ROLE, execution.role('ForwardDepth', 'fragment'))
        self.assertEqual('DeferredLightingFragment', execution.program_member('DeferredLighting', 'fragment'))
        self.assertEqual('DeferredOpaqueGBufferFragment', execution.program_member('DeferredOpaqueGBuffer', 'fragment'))

    def test_baked_lighting_appends_the_chart_g_buffer_role_after_the_probe_role(self):
        # Version 37 appends the chart G-buffer after version 36's probe G-buffer without renumbering either.
        probe = execution.role('DeferredProbeGBuffer', 'fragment')
        self.assertEqual(execution.role('ForwardSurfaceChart', 'fragment') + 2, probe)
        self.assertEqual(probe + 1, execution.role('DeferredSurfaceChartGBuffer', 'fragment'))
        self.assertEqual('DeferredSurfaceChartGBufferFragment',
                         execution.program_member('DeferredSurfaceChartGBuffer', 'fragment'))
        self.assertIn('DeferredSurfaceChartGBuffer', forward.BAKED_LIGHTING_GROUPS)
        self.assertIn('DeferredSurfaceChartGBuffer', forward.GBUFFER_GROUPS)
        # The role first appears in version 37; later generations keep it.
        self.assertGreaterEqual(execution.PACKAGE_VERSION, 37)

    def test_connected_baked_lighting_wraps_the_material_in_the_chart_g_buffer(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = graph.new_workspace(root / 'sources', output='offscreen', profiles=('metal',),
                                         renderer_static=True)
            helper = ray_tests.stage_helper(root / 'helpers')
            expanded = {}
            original = forward.expand

            def record(programs, techniques, snapshot, helper, build_module, has_surface_charts=False, **options):
                expanded['charts'] = has_surface_charts
                expanded['result'] = original(programs, techniques, snapshot, helper, build_module, has_surface_charts,
                                              **options)
                return expanded['result']

            # The forward profile lowers its LUSL recipes, so the build needs a luslc.
            environment = ray_tests.stand_in_luslc(root)
            if environment is None:
                self.skipTest('set LUDUS_LUSLC to lower the LUSL forward recipes')
            with patch.object(forward, 'expand', side_effect=record), patch.dict(os.environ, environment):
                build.prepare_sources(source.with_suffix('').with_suffix('.lushader.json'), helper)
            # The Raster starter connects Baked Lighting, so both baked G-buffer wrappers are generated; the
            # forward chart fragment is not, because the deferred G-buffer consumes the chart.
            self.assertTrue(expanded['charts'])
            result = expanded['result']
            self.assertNotIn('ForwardSurfaceChartFragment', result)
            chart = result['DeferredSurfaceChartGBufferFragment']
            self.assertEqual('DeferredSurfaceChartGBuffer', chart['forward_group'])
            self.assertTrue(chart['forward_wrapper'].startswith('#define LUDUS_GRAPH_SURFACE_CHART 1\n'))
            self.assertIn('PackDeferredSurfaceChartMaterial(MaterialMain(value), value)', chart['forward_wrapper'])
            self.assertIn('PackDeferredProbeMaterial(', result['DeferredProbeGBufferFragment']['forward_wrapper'])

if __name__ == '__main__':
    unittest.main()
