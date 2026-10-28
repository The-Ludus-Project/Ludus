"""Deferred callback selection and stable cooked-role ABI regressions."""
import json
import unittest
from unittest.mock import patch

import test_render_graph_build as fixtures
from ludus_tools import render_graph as graph
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

            def inspect(programs, techniques, *_):
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
        expected = [('DeferredGBuffer', 'fragment'), ('DeferredLighting', 'vertex'),
                    ('DeferredLighting', 'fragment'), ('DeferredDepthCopy', 'fragment')]
        self.assertEqual([first + index for index in range(len(expected))],
                         [execution.role(*key) for key in expected])
        self.assertEqual(execution.FORWARD_DEPTH_STAGE_ROLE, execution.role('ForwardDepth', 'fragment'))
        self.assertEqual('DeferredLightingFragment', execution.program_member('DeferredLighting', 'fragment'))


if __name__ == '__main__':
    unittest.main()
