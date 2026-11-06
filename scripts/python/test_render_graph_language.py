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

# A stand-in luslc that records its arguments and the files of its snapshot, then writes a module
# and a report with one specialization per `template` line of the source.
FAKE_LUSLC = textwrap.dedent('''\
    #!{python}
    import json, pathlib, sys
    arguments = sys.argv[1:]
    root = pathlib.Path(arguments[arguments.index('--root') + 1])
    output = pathlib.Path(arguments[arguments.index('--output') + 1])
    report = pathlib.Path(arguments[arguments.index('--report') + 1])
    if 'fail' in arguments[-1]:
        sys.stderr.write(arguments[-1] + '(3,5): error LUSL0504 type-mismatch: expected uint32\\n')
        sys.exit(1)
    files = sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file())
    output.write_text('// lowered ' + json.dumps({{'arguments': arguments, 'files': files}}) + '\\n')
    source = (root / arguments[-1]).read_text()
    lines = [f'specialization\\t{{arguments[-1]}}:1\\tShade<T{{i}}>\\t_luslSpecialization{{i}}_Shade'
             for i in range(source.count('template'))]
    if 'groupshared' in source:
        lines.append(f'groupshared-clear\\t{{arguments[-1]}}:2\\t{{arguments[-1]}}:3\\t1024')
    report.write_text(''.join(line + '\\n' for line in lines))
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
        tool, reports = language.lower_sources(snapshot, programs, self.helper, build, luslc=self.luslc, **options)
        self.reports = reports
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
        self.assertEqual(language.lower_sources(snapshot, programs, self.helper, build, luslc=None), (None, {}))
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

    def test_reports_specializations_and_checks_the_package_budget(self):
        self.write('main_shaders/miss.lusl', 'template\ntemplate\ngroupshared\n')
        self.write('main_shaders/closest.lusl', 'template\n')
        programs = {'Miss': {'source': 'main_shaders/miss.lusl'}, 'Closest': {'source': 'main_shaders/closest.lusl'}}
        snapshot, _ = self.lower(programs)
        self.assertNotIn('--max-specializations', snapshot.files['main_shaders/miss.lusl.slang'].decode())
        self.assertEqual([record['name'] for record in self.reports['main_shaders/miss.lusl']['specializations']],
                         ['Shade<T0>', 'Shade<T1>'])
        self.assertEqual(self.reports['main_shaders/miss.lusl']['groupshared_clears'],
                         [{'entry': 'main_shaders/miss.lusl:2', 'variable': 'main_shaders/miss.lusl:3', 'bytes': 1024}])
        self.assertEqual(len(self.reports['main_shaders/closest.lusl']['specializations']), 1)
        programs = {'Miss': {'source': 'main_shaders/miss.lusl'}, 'Closest': {'source': 'main_shaders/closest.lusl'}}
        snapshot, _ = self.lower(programs, specialization_budget=3)
        self.assertIn('"--max-specializations", "3"', snapshot.files['main_shaders/miss.lusl.slang'].decode())
        # Each source fits on its own; together they need one more than the package declares.
        programs = {'Miss': {'source': 'main_shaders/miss.lusl'}, 'Closest': {'source': 'main_shaders/closest.lusl'}}
        with self.assertRaises(ToolingError) as budget:
            self.lower(programs, specialization_budget=2)
        message = str(budget.exception)
        self.assertIn('need 3 template specializations together', message)
        self.assertIn('increase budgets.specializations to at least 3', message)
        self.assertIn('main_shaders/miss.lusl has the most', message)

    def test_manifest_budget_of_specializations_is_optional_and_positive(self):
        manifest = {'version': 1, 'id': 'Graph', 'profiles': ['spirv'], 'schemas': [], 'programs': [],
                    'techniques': [], 'budgets': {'configurations': 1, 'programs': 1}}
        for budget in (0, -1, 'many'):
            manifest['budgets']['specializations'] = budget
            with self.assertRaises(ToolingError) as invalid:
                build.validate_manifest(manifest)
            self.assertIn('budget specializations', str(invalid.exception))
        # A positive budget passes the budget checks and fails later, on the empty programs.
        manifest['budgets']['specializations'] = 4
        with self.assertRaises(ToolingError) as later:
            build.validate_manifest(manifest)
        self.assertNotIn('budget specializations', str(later.exception))

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
