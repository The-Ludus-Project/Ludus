"""Tests of LUSL program sources in render-graph builds (ludus_tools.render_graph_language)."""

import os
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ludus_tools import render_graph_build as build  # noqa: E402
from ludus_tools import render_graph_language as language  # noqa: E402
from ludus_tools.errors import ToolingError  # noqa: E402

# A stand-in luslc that records its arguments and the files of its snapshot, then writes a module.
FAKE_LUSLC = textwrap.dedent('''\
    #!{python}
    import json, pathlib, sys
    arguments = sys.argv[1:]
    root = pathlib.Path(arguments[arguments.index('--root') + 1])
    output = pathlib.Path(arguments[arguments.index('--output') + 1])
    if 'fail' in arguments[-1]:
        sys.stderr.write(arguments[-1] + '(3,5): error LUSL0504 type-mismatch: expected uint32\\n')
        sys.exit(1)
    files = sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file())
    output.write_text('// lowered ' + json.dumps({{'arguments': arguments, 'files': files}}) + '\\n')
''')


@unittest.skipIf(os.name == 'nt', 'the stand-in luslc is a POSIX script')
class RenderGraphLanguageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        base = Path(self.directory.name)
        self.root = base / 'graph'
        self.helper = base / 'helper'
        self.root.mkdir()
        self.helper.mkdir()
        self.luslc = base / 'luslc'
        self.luslc.write_text(FAKE_LUSLC.format(python=sys.executable))
        self.luslc.chmod(self.luslc.stat().st_mode | stat.S_IXUSR)
        (self.helper / 'ray_shader.luslh').write_text('#pragma once\nextern slang\n{\n};\n')
        (self.helper / 'math.luslh').write_text('#pragma once\nextern slang "math.slang"\n{\n};\n')
        (self.helper / 'math.slang').write_text('static const float PI = 3.14;\n')

    def tearDown(self):
        self.directory.cleanup()

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)

    def lower(self, programs, **options):
        snapshot = build.Snapshot(self.root)
        tool = language.lower_sources(snapshot, programs, self.helper, build, luslc=self.luslc, **options)
        return snapshot, tool

    def test_lowers_each_source_once_and_points_programs_at_the_module(self):
        self.write('main_shaders/common.luslh', '#include "ray_shader.luslh"\n#include "math.luslh"\n')
        self.write('main_shaders/miss.lusl', '#include "common.luslh"\nfloat32x3 ShadeMiss(in Ray ray);\n')
        self.write('main_shaders/unused.luslh', '')
        programs = {'Miss': {'source': 'main_shaders/miss.lusl'}, 'Other': {'source': 'main_shaders/miss.lusl'},
                    'Slang': {'source': 'main_shaders/color.slang'}}
        snapshot, tool = self.lower(programs, style='warn')
        self.assertEqual(tool, self.luslc)
        generated = 'main_shaders/miss.lusl.slang'
        self.assertEqual(programs['Miss']['source'], generated)
        self.assertEqual(programs['Miss']['language_source'], 'main_shaders/miss.lusl')
        self.assertEqual(programs['Other']['source'], generated)
        self.assertEqual(programs['Slang']['source'], 'main_shaders/color.slang')
        self.assertIn(generated, snapshot.generated)
        text = snapshot.files[generated].decode()
        # Headers resolve beside the file, from the root, then from the SDK directory.
        for path in ('main_shaders/miss.lusl', 'main_shaders/common.luslh', '_ludus_ray/ray_shader.luslh',
                     '_ludus_ray/math.luslh', '_ludus_ray/math.slang'):
            self.assertIn('"' + path + '"', text)
        self.assertNotIn('unused.luslh', text)
        self.assertIn('"--include-directory", "_ludus_ray"', text)
        self.assertIn('"--style", "warn"', text)
        # SDK headers join the snapshot as engine files; the generated module is not re-read.
        self.assertEqual(snapshot.external['_ludus_ray/ray_shader.luslh'], self.helper / 'ray_shader.luslh')
        snapshot.verify()

    def test_graphs_without_lusl_need_no_luslc(self):
        snapshot = build.Snapshot(self.root)
        programs = {'Color': {'source': 'main_shaders/color.slang'}}
        self.assertIsNone(language.lower_sources(snapshot, programs, self.helper, build, luslc=None))
        self.assertEqual(snapshot.files, {})

    def test_reports_luslc_failures_missing_tools_and_bad_styles(self):
        self.write('main_shaders/fail.lusl', 'void Broken();\n')
        with self.assertRaises(ToolingError) as failure:
            self.lower({'Broken': {'source': 'main_shaders/fail.lusl'}})
        self.assertIn('error LUSL0504', str(failure.exception))
        self.write('main_shaders/miss.lusl', '')
        with self.assertRaises(ToolingError) as style:
            self.lower({'Miss': {'source': 'main_shaders/miss.lusl'}}, style='loose')
        self.assertIn('strict, warn, off', str(style.exception))
        snapshot = build.Snapshot(self.root)
        previous = os.environ.pop('LUDUS_LUSLC', None)
        try:
            with self.assertRaises(ToolingError) as missing:
                language.lower_sources(snapshot, {'Miss': {'source': 'main_shaders/miss.lusl'}}, self.helper, build)
        finally:
            if previous is not None:
                os.environ['LUDUS_LUSLC'] = previous
        self.assertIn('need luslc', str(missing.exception))

    def test_generated_module_cannot_replace_an_authored_file(self):
        self.write('main_shaders/miss.lusl', '')
        self.write('main_shaders/miss.lusl.slang', '// authored\n')
        snapshot = build.Snapshot(self.root)
        snapshot.load('main_shaders/miss.lusl.slang')
        with self.assertRaises(ToolingError):
            language.lower_sources(snapshot, {'Miss': {'source': 'main_shaders/miss.lusl'}}, self.helper, build,
                                   luslc=self.luslc)

    def test_generated_closure_keeps_line_directives_and_sdk_includes(self):
        snapshot = build.Snapshot(self.root)
        snapshot.capture('main_shaders/miss.lusl.slang',
                         b'#include "../_ludus_ray/ray_shader.slang"\n#line 4 "main_shaders/miss.lusl"\n'
                         b'float3 ShadeMiss(Ray ray) { return float3(0); }\n', generated=True)
        self.assertEqual(snapshot.shader_closure('main_shaders/miss.lusl.slang'),
                         ['_ludus_ray/ray_shader.slang', 'main_shaders/miss.lusl.slang'])
        # Authored Slang still may not remap lines.
        self.write('main_shaders/remap.slang', '#line 4 "elsewhere.slang"\n')
        with self.assertRaises(ToolingError):
            snapshot.shader_closure('main_shaders/remap.slang')


if __name__ == '__main__':
    unittest.main()
