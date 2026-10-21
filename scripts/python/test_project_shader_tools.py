"""Project readiness and repair own graph shader dependencies; opening never downloads."""
import json
import sys
import unittest
from pathlib import Path

from ludus_tools.errors import ToolingError
from ludus_tools.shader_tools import project_shader_tools, required_shader_tools
import test_macos_project_tooling as mac_fixtures
from test_project_setup import write_translator
from ludus_tools import project_setup as setup

ROOT = Path(__file__).resolve().parents[2]


class ProjectShaderToolsTests(unittest.TestCase):
    def setUp(self):
        self.fixture = mac_fixtures.MacosProjectSetupTests('runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.project = self.fixture.project
        self.tools = self.fixture.tools
        self.sdk = self.fixture.sdk
        self.calls = []
        python = self.tools / 'out/host-tools/venv/bin/python'
        python.symlink_to(sys.executable)

    def add_graph(self, profiles=('metal',), *, has_rays=True):
        source = ROOT / ('tests/sdk_consumer/rg2/raytracing/main.lushader.json.in' if has_rays else
                         'tests/sdk_consumer/rg2/offscreen/main.lushader.json.in')
        manifest = self.project / 'graphs/Main/main.lushader.json'
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(source.read_text().replace('@RG2_TEST_PROFILES@', json.dumps(list(profiles))))
        return manifest

    def write_compiler(self):
        compiler = self.tools / 'out/shader-tools/slang/bin/slangc'
        compiler.parent.mkdir(parents=True, exist_ok=True)
        compiler.write_text('#!/bin/sh\nexit 0\n')
        compiler.chmod(0o755)
        return compiler

    def runner(self, argv, *, cwd, env):
        self.calls.append(list(argv))
        if len(argv) > 1 and Path(argv[1]).name == 'bootstrap-spirv-cross':
            self.assertEqual(cwd, self.tools)
            write_translator(self.tools, self.sdk)
            return ''
        if len(argv) > 1 and Path(argv[1]).name == 'shader-probe':
            self.assertEqual(cwd, self.tools)
            self.write_compiler()
            return ''
        return self.fixture.runner(argv, cwd=cwd, env=env)

    def repair(self, **options):
        return setup.repair_project(self.project, tooling_root=self.tools, sdk=self.sdk,
                                    runner=self.runner, **options)

    def preparations(self):
        return [Path(argv[1]).name for argv in self.calls if len(argv) > 1 and
                Path(argv[1]).name in ('shader-probe', 'bootstrap-spirv-cross')]

    def test_new_ray_graph_invalidates_ready_setup_without_writing_or_preparing(self):
        self.repair()
        self.add_graph()
        self.write_compiler()
        before = self.fixture.snapshot()
        self.calls.clear()
        with self.assertRaisesRegex(ToolingError, 'prepare them automatically'):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.assertEqual(before, self.fixture.snapshot())
        self.assertEqual(self.preparations(), [])
        self.assertEqual(self.calls, [])

    def test_repair_prepares_metal_ray_translator_and_reuses_it_on_repeat(self):
        self.add_graph()
        self.write_compiler()
        self.repair()
        self.assertEqual(self.preparations(), ['bootstrap-spirv-cross'])
        cache = setup.read_object(self.project / 'CMakeUserPresets.json')['configurePresets'][0]['cacheVariables']
        self.assertEqual(cache['LUDUS_SPIRV_CROSS'], str(self.tools / 'out/shader-tools/spirv-cross/bin/spirv-cross'))
        self.assertNotIn('LUDUS_SPIRV_VALIDATOR', cache)
        self.assertEqual(cache['CMAKE_CXX_COMPILER'], str(self.tools / 'out/host-tools/bin/clang++'))
        self.calls.clear()
        self.repair()
        self.assertEqual(self.preparations(), [])
        setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)

    def test_fresh_repair_prepares_all_required_tools_before_the_project_build(self):
        self.add_graph()
        self.repair()
        self.assertEqual(self.preparations(), ['shader-probe', 'bootstrap-spirv-cross'])
        prepared = [index for index, argv in enumerate(self.calls) if len(argv) > 1 and
                    Path(argv[1]).name in ('shader-probe', 'bootstrap-spirv-cross')]
        builds = [index for index, argv in enumerate(self.calls) if '--build' in argv]
        self.assertTrue(builds)
        self.assertLess(max(prepared), min(builds))

    def test_failed_and_cancelled_preparation_keep_project_settings(self):
        self.repair()
        self.add_graph()
        self.write_compiler()
        before = self.fixture.snapshot()
        def fail(*args, **kwargs):
            raise ToolingError('ConfigureFailed', 'download failed')
        with self.assertRaisesRegex(ToolingError, 'download failed'):
            setup.repair_project(self.project, tooling_root=self.tools, sdk=self.sdk, runner=fail)
        self.assertEqual(self.fixture.snapshot(), before)
        def cancel():
            raise ToolingError('Cancelled', 'cancelled')
        with self.assertRaisesRegex(ToolingError, 'cancelled'):
            self.repair(cancel_check=cancel)
        self.assertEqual(self.fixture.snapshot(), before)
        self.assertEqual(self.preparations(), [])

    def test_changed_translator_binary_and_source_pin_require_repair(self):
        self.add_graph()
        self.repair()
        binary = self.tools / 'out/shader-tools/spirv-cross/bin/spirv-cross'
        binary.write_bytes(binary.read_bytes() + b'\n# changed\n')
        with self.assertRaisesRegex(ToolingError, 'Rendering tools need preparation'):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.calls.clear()
        self.repair()
        self.assertEqual(self.preparations(), ['bootstrap-spirv-cross'])
        stamp = Path(str(binary) + '.build.json')
        value = json.loads(stamp.read_text())
        value['commit'] = 'stale'
        stamp.write_text(json.dumps(value))
        with self.assertRaises(ToolingError):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)

    def test_incompatible_selected_sdk_pin_cannot_be_reported_ready(self):
        self.add_graph()
        self.write_compiler()
        binary = write_translator(self.tools, self.sdk)
        pin = self.sdk / 'lib/cmake/Ludus/shaders/spirv_cross_toolchain.json'
        value = json.loads(pin.read_text())
        value['spirv_cross']['commit'] = 'other-sdk'
        pin.write_text(json.dumps(value))
        def leave_incompatible(argv, **kwargs):
            return ''
        before = self.fixture.snapshot()
        with self.assertRaises(ToolingError):
            setup.repair_project(self.project, tooling_root=self.tools, sdk=self.sdk, runner=leave_incompatible)
        self.assertTrue(binary.is_file())
        self.assertEqual(self.fixture.snapshot(), before)
        self.assertFalse((self.project / '.ludus/setup.json').exists())

    def test_plain_metal_raster_needs_neither_translator_nor_validator(self):
        self.add_graph(has_rays=False)
        self.repair()
        self.assertEqual(self.preparations(), ['shader-probe'])
        self.assertFalse((self.tools / 'out/shader-tools/spirv-cross').exists())

    def test_invalid_and_external_graphs_do_not_execute_or_expand_tool_requirements(self):
        manifest = self.add_graph()
        manifest.write_text('{}')
        self.assertEqual(project_shader_tools(self.project, self.project, self.fixture.profile, False), set())
        external = self.fixture.root / 'external.lushader.json'
        external.write_bytes((ROOT / 'tests/sdk_consumer/rg2/raytracing/main.lushader.json.in').read_bytes())
        manifest.unlink()
        manifest.symlink_to(external)
        self.assertEqual(project_shader_tools(self.project, self.project, self.fixture.profile, False), set())

    def test_dependency_selection_is_shared_with_graph_builds(self):
        self.assertEqual(required_shader_tools({'profiles': ['metal'], 'techniques': []}), {'compiler'})
        self.assertEqual(required_shader_tools({'profiles': ['spirv'], 'techniques': []}), {'compiler', 'validator'})
        self.assertEqual(required_shader_tools({'profiles': ['glsl'], 'techniques': []}),
                         {'compiler', 'validator', 'spirv_cross'})


if __name__ == '__main__':
    unittest.main()
