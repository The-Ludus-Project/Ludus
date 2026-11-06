"""Tests of the SDK .luslh generator (cmake/shaders/generate_luslh.py)."""

import importlib.util
import os
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HELPER = Path(__file__).resolve().parents[2] / 'cmake' / 'shaders' / 'generate_luslh.py'
SPECIFICATION = importlib.util.spec_from_file_location('generate_luslh', HELPER)
generate_luslh = importlib.util.module_from_spec(SPECIFICATION)
SPECIFICATION.loader.exec_module(generate_luslh)

# A stand-in lusl-declarations that prints fixed records and the files it was given.
RECORDS = '\n'.join([
    'constant\tPI\tfloat32',
    'struct\tPair',
    'field\tFirst\tuint32x2',
    'field\tSecond\tfloat32',
    'function\tIsFinite\tbool',
    'parameter\tvalue\tin\tfloat32',
    'function\tIsFinite\tbool',
    'parameter\tvalue\tin\tfloat32x3',
    'function\tSplit\tvoid',
    'parameter\tvalue\tin\tuint32',
    'parameter\toutLow\tout\tuint32',
    'parameter\tinoutHigh\tinout\tuint32',
    'function\tUseTexture\tfloat32x4',
    'parameter\timage\tin\t?Texture2D<float4>',
    'function\tLong\tfloat32x3',
    *[f'parameter\tveryLongParameterName{i}\tin\tfloat32x3' for i in range(4)],
])
FAKE_TOOL = textwrap.dedent('''\
    #!{python}
    import sys
    arguments = sys.argv[1:]
    sys.stdout.write({records!r} + '\\n')
    sys.stderr.write(' '.join(arguments))
''')


@unittest.skipIf(os.name == 'nt', 'the stand-in tool is a POSIX script')
class GenerateLuslhTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.tool = self.root / 'tool'
        self.tool.write_text(FAKE_TOOL.format(python=sys.executable, records=RECORDS))
        self.tool.chmod(self.tool.stat().st_mode | stat.S_IXUSR)
        (self.root / 'math.slang').write_text('#include "detail.slang"\n')
        (self.root / 'detail.slang').write_text('')

    def tearDown(self):
        self.directory.cleanup()

    def expand(self, body):
        template = self.root / 'math.luslh.in'
        template.write_text(body)
        return generate_luslh.expand(template, self.tool)

    def test_spells_every_kind_and_keeps_comments(self):
        text = self.expand('// Header comment.\n#pragma once\n\n@extern "math.slang"\n@declare PI\n\n'
                           '/// Two values.\n@declare Pair\n@declare IsFinite Split\n@end\n')
        self.assertTrue(text.startswith('// Generated from math.luslh.in'))
        self.assertIn('// Header comment.\n#pragma once\n\nextern slang "math.slang"\n{\n', text)
        self.assertIn('    static const float32 PI;\n\n    /// Two values.\n', text)
        self.assertIn('    struct Pair\n    {\n        uint32x2 First;\n        float32 Second;\n    };\n', text)
        self.assertIn('    bool IsFinite(in float32 value);\n    bool IsFinite(in float32x3 value);\n', text)
        self.assertIn('    void Split(in uint32 value, out uint32 outLow, inout uint32 inoutHigh);\n};\n', text)

    def test_path_less_blocks_and_long_signatures(self):
        text = self.expand('@extern from "math.slang"\n@declare Long\n@end\n')
        self.assertIn('extern slang\n{\n', text)
        self.assertIn('    float32x3 Long(in float32x3 veryLongParameterName0,\n'
                      '                   in float32x3 veryLongParameterName1,\n', text)
        self.assertIn('                   in float32x3 veryLongParameterName3);\n', text)

    def test_includes_reach_the_tool(self):
        files = generate_luslh.collect(self.root, 'math.slang')
        self.assertEqual(files, ['math.slang', 'detail.slang'])

    def test_template_errors_name_the_line(self):
        cases = {
            '@declare PI\n': 'inside an @extern block',
            '@extern "math.slang"\n@declare Missing\n@end\n': 'declares no function, struct or constant Missing',
            '@extern "math.slang"\n@declare PI PI\n@end\n': 'declared twice',
            '@extern "math.slang"\n@declare UseTexture\n@end\n': 'LUSL cannot declare yet',
            '@extern "math.slang"\n@declare PI\n': 'has no @end',
            '@frobnicate\n': 'unknown directive',
        }
        for body, message in cases.items():
            with self.subTest(body=body):
                with self.assertRaises(generate_luslh.TemplateError) as failure:
                    self.expand(body)
                self.assertIn(message, str(failure.exception))

    def test_check_reports_stale_headers(self):
        template = self.root / 'math.luslh.in'
        template.write_text('@extern "math.slang"\n@declare PI\n@end\n')
        header = self.root / 'math.luslh'
        header.write_text('stale\n')
        sys.argv = ['generate_luslh.py', '--check', '--tool', str(self.tool), str(template)]
        self.assertEqual(generate_luslh.main(), 1)
        self.assertEqual(header.read_text(), 'stale\n')
        sys.argv = ['generate_luslh.py', '--tool', str(self.tool), str(template)]
        self.assertEqual(generate_luslh.main(), 0)
        sys.argv = ['generate_luslh.py', '--check', '--tool', str(self.tool), str(template)]
        self.assertEqual(generate_luslh.main(), 0)


if __name__ == '__main__':
    unittest.main()
