"""Regression checks for formatting and preservation of partial staging."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import engine
import formatting
from formatting import FormattingError, format_source, significant_tokens
import pre_commit


ROOT = Path(__file__).resolve().parents[2]
CLANG_FORMAT = shutil.which(engine.pinned_llvm_tool(ROOT, "clang-format"))


def comparable_tokens(source):
    """Return tokens without trailing commas, joining strings clang-format may split."""
    tokens = []
    for token in significant_tokens(source):
        if token.startswith('"') and tokens and tokens[-1].startswith('"'):
            tokens[-1] = tokens[-1][:-1] + token[1:]
        else:
            tokens.append(token)
    comparable = []
    for index, token in enumerate(tokens):
        following = next((other for other in tokens[index + 1:] if not other.startswith(('//', '/*'))), None)
        if token != ',' or following != '}':
            comparable.append(token)
    return comparable


@unittest.skipUnless(CLANG_FORMAT, f"requires pinned {engine.pinned_llvm_tool(ROOT, 'clang-format')}")
class FormattingTests(unittest.TestCase):
    def formatted(self, source):
        result = format_source(source, ROOT / "apps/smoke/main.cpp", CLANG_FORMAT)
        # Formatting may change whitespace and add trailing commas, never other tokens.
        self.assertEqual(comparable_tokens(result), comparable_tokens(source))
        return result

    def test_designated_initializer_and_idempotence(self):
        source = 'void f() { const Info value = {.Name = "test", .Width = 800}; }\n'
        result = self.formatted(source)
        self.assertIn('const Info value = { .Name = "test", .Width = 800 };', result)
        self.assertEqual(self.formatted(result), result)

    def test_the_two_allowed_layouts_and_the_forbidden_layout(self):
        compact = 'Info str = { .a = 0, .b = 1 };\n'
        multiline = 'Info str =\n{\n    .a = 0,\n    .b = 1,\n};\n'
        mixed = 'Info str = {\n    .a = 0,\n    .b = 1,\n};\n'
        self.assertEqual(self.formatted(compact), compact)
        self.assertEqual(self.formatted(multiline), multiline)
        self.assertEqual(self.formatted(mixed), multiline)

    def test_multiline_initializer_brace_is_on_its_own_line(self):
        source = 'void f() { const Info value = {\n.Name = "test",\n.Width = 800\n}; }\n'
        result = self.formatted(source)
        self.assertIn('const Info value =\n    {\n        .Name = "test",\n        .Width = 800,\n    };', result)
        self.assertNotIn('value = {\n', result)
        self.assertEqual(self.formatted(result), result)

    def test_nested_and_direct_initializers(self):
        result = self.formatted('Info value{\n.Child = {\n.Value = 1\n},\n.Other = 2\n};\n')
        self.assertIn('Info value\n{', result)
        self.assertIn('.Child =\n    {\n        .Value = 1,\n    },', result)
        self.assertEqual(self.formatted(result), result)

    def test_literals_comments_macros_and_disabled_regions(self):
        source = '''// clang-format off
Info untouched = {.Value = 1};
const char* text = R"tag({.Fake = "text"})tag";
// {.Fake = 1}
#define VALUE {.Value = 1}
// clang-format on
Info value = {/* {.Fake = 2} */ .Value = 3};
'''
        result = self.formatted(source)
        self.assertTrue(result.startswith(source[:source.index('// clang-format on')]))
        self.assertIn('.Value = 3', result)
        self.assertEqual(self.formatted(result), result)

    def test_numeric_separator_and_ordinary_initializers(self):
        result = self.formatted("Info value = {.Value = 1'000};\nint a[] = {1, 2};\n")
        self.assertIn("{ .Value = 1'000 }", result)
        self.assertIn('int a[] = {1, 2};', result)

    def test_constructor_member_initializer_indentation(self):
        result = self.formatted('struct C { C() : first(1), info{\n.Width = 1,\n.Height = 2\n} {} };\n')
        self.assertIn('info\n        {\n            .Width = 1,\n            .Height = 2,\n        }', result)
        self.assertEqual(self.formatted(result), result)

    def test_long_compact_initializer_expands(self):
        name = 'x' * 130
        result = self.formatted('Info value = {.Name = "' + name + '", .Width = 800};\n')
        self.assertIn('Info value =\n{\n', result)
        self.assertIn('.Width = 800,\n};', result)
        self.assertEqual(self.formatted(result), result)

    def test_compact_spaces_respect_the_column_limit(self):
        for nested in (False, True):
            prefix = 'Info value = {.Child = {.Name = "' if nested else 'Info value = {.Name = "'
            suffix = '"}};\n' if nested else '"};\n'
            source = prefix + 'x' * (120 - len(prefix) - len(suffix.rstrip())) + suffix
            result = self.formatted(source)
            self.assertIn('Info value =\n{\n', result)
            self.assertEqual(self.formatted(result), result)

    def test_raw_string_contents_are_preserved(self):
        literal = 'R"tag(first\n    {.Fake = 2}\nlast)tag"'
        result = self.formatted('Info value{.Text = ' + literal + ', .Width = 1};\n')
        self.assertIn(literal, result)
        self.assertEqual(self.formatted(result), result)

    def test_macro_arguments_and_positional_outer_aggregate(self):
        result = self.formatted('void f() { REQUIRE(Create({.Name = "test"}, window)); '
                                'return {Kind::Signed, {.Signed = 1}}; }\n')
        self.assertEqual(self.formatted(result), result)

    def test_initializer_comments_are_preserved(self):
        for source in ('Info value = {.Value = 1 /* tail */};\n',
                       'Info value = {.Value = 1, /* tail */};\n',
                       'Info value = {/* tail */ .Value = 1};\n',
                       'Info value = {.Value = 1 // tail\n};\n'):
            with self.subTest(source=source):
                result = self.formatted(source)
                self.assertIn('/* tail */' if '/*' in source else '// tail', result)
                self.assertEqual(self.formatted(result), result)

    def test_closing_brace_after_last_member_keeps_every_member(self):
        # Clang-format 18 keeps a ternary operand's closing brace on its last member's line.
        source = '''struct Pair final { int First = 0; int Second = 0; };
Pair Choose(bool isFirst, int a, int b) noexcept
{
    const Pair other = isFirst == true ? Pair{ .First = a,
                                               .Second = b }
                                       : Pair{ .First = b,
                                               .Second = a };
    return other;
}
'''
        result = self.formatted(source)
        for member in ('.First = a', '.Second = b', '.First = b', '.Second = a'):
            self.assertIn(member, result)
        self.assertEqual(self.formatted(result), result)

    def test_token_changing_normalization_fails_loudly(self):
        source = 'Info value = {\n.Name = "test",\n.Width = 800\n};\n'
        with patch.object(formatting, 'significant_tokens', side_effect=[['before'], ['after']]):
            with self.assertRaises(FormattingError):
                self.formatted(source)

    def test_hook_formats_index_and_preserves_unstaged_edits(self):
        for partially_staged in (False, True):
            with self.subTest(partially_staged=partially_staged), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                def git(*args):
                    return subprocess.run(['git', *args], cwd=root, check=True, capture_output=True).stdout
                git('init', '-q')
                shutil.copy(ROOT / '.clang-format', root / '.clang-format')
                path = root / 'apps/example.cpp'
                path.parent.mkdir()
                staged = 'Info value = {.Value = 1};\n'
                path.write_text(staged)
                git('add', 'apps/example.cpp')
                working = staged + '// unstaged change\n' if partially_staged else staged
                path.write_text(working)
                previous = Path.cwd()
                try:
                    os.chdir(root)
                    with patch.object(pre_commit, 'repo_root', return_value=root), \
                            patch.object(pre_commit, 'load_tool_versions', return_value={}), \
                            patch.object(pre_commit, 'find_system_tool', return_value=CLANG_FORMAT):
                        pre_commit.main()
                finally:
                    os.chdir(previous)
                expected = format_source(staged, path, CLANG_FORMAT)
                self.assertEqual(git('show', ':apps/example.cpp').decode(), expected)
                self.assertEqual(path.read_text(), working if partially_staged else expected)


if __name__ == '__main__':
    unittest.main()
