"""Generate the SDK's LUSL `.luslh` headers from Slang reflection.

Each header has a template, `<name>.luslh.in`, beside it. The template is the header with its
`extern slang` blocks and macro constants written as directives:

    @search "../../foundation/math/shaders"   another directory includes resolve in
    @extern "math.slang"                      a block that names math.slang (lowering includes it)
    @extern from "scene_shader.slang"         a path-less block; the file is only reflected
    /// One addressed draw of the counter-based random generator.
    @declare RandomAddress                    a struct, enum, constant or function (every overload)
    @declare PI ComputeLuminance              several names, in order
    @end                                      closes the block
    @macro uint32 SCENE_FAILURE_AUTHORED_FIRST = LUDUS_SCENE_FAILURE_AUTHORED_FIRST from "scene_shader_layout.h"
    @prelude #define LUDUS_SCENE_RAY_INTERFACE 1   Slang text placed before each reflected file

`@prelude` lines reproduce what a generated wrapper declares before the file, such as the
uniforms scene_shader.slang reads, so the file reflects as the wrapper compiles it.

Other lines, such as doc comments or hand-written declarations of functions that generated
wrappers define, are copied. Includes resolve by file name in the template's directory, then each
@search directory, as the render-graph build stages its flat helper directory; a name found in two
places is an error. `@macro` reflects the value of a C macro through a probe module and emits a
LUSL constant: macros are not Slang declarations, and a LUSL name equal to the macro would be
replaced by the preprocessor of the generated Slang.

Declarations are spelled from what `lusl-declarations` reflects, so a header cannot drift from
the Slang it declares (docs/modules/tooling/shader-language.md#sdk-declarations). `--check`
reports stale headers; without it the headers are rewritten.
"""
import argparse
import posixpath
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 120
# The repository's .clang-format ColumnLimit; longer signatures wrap one parameter per line.
COLUMN_LIMIT = 120
INDENT = '    '
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"\n]+)"', re.MULTILINE)
EXTERN = re.compile(r'@extern(?:[ \t]+(from))?[ \t]+"([^"\n]+)"[ \t]*\Z')
SEARCH = re.compile(r'@search[ \t]+"([^"\n]+)"[ \t]*\Z')
MACRO = re.compile(r'@macro[ \t]+(int32|uint32)[ \t]+([A-Z][A-Z0-9_]*)[ \t]*=[ \t]*([A-Za-z_][A-Za-z0-9_]*)'
                   r'[ \t]+from[ \t]+"([^"\n]+)"[ \t]*\Z')
# The module that reflects macro values: one `static const` per macro of one file.
PROBE = '_lusl_macro_probe.slang'
PROBE_PREFIX = '_luslMacro'
SLANG_TYPES = {'int32': 'int', 'uint32': 'uint'}


class TemplateError(Exception):
    pass


def collect(root, source):
    """`source` and the files it includes, transitively, as paths relative to `root`."""
    found, pending = [], [source]
    while pending:
        relative = posixpath.normpath(pending.pop())
        if relative in found or relative.startswith('../') or not (root / relative).is_file():
            continue
        found.append(relative)
        directory = posixpath.dirname(relative)
        for name in INCLUDE.findall((root / relative).read_text(encoding='utf-8', errors='replace')):
            pending += [posixpath.join(directory, name), name]
    return found


def stage(roots, source, destination, extra=()):
    """Copy `source`, the `extra` names and their includes, found by file name in `roots`, flat
    into `destination`. Returns the staged names."""
    pending, staged = [source, *extra], set()
    while pending:
        name = posixpath.basename(pending.pop())
        if name in staged:
            continue
        owners = [root / name for root in roots if (root / name).is_file()]
        if len(owners) > 1:
            raise TemplateError(f'{name} is found in {len(owners)} search directories; each name needs one owner')
        if not owners:
            continue
        staged.add(name)
        text = owners[0].read_text(encoding='utf-8', errors='replace')
        (destination / name).write_text(text, encoding='utf-8')
        pending += INCLUDE.findall(text)
    if posixpath.basename(source) not in staged:
        raise TemplateError(f'{source} is not in the template directory or a @search directory')
    return sorted(staged)


def reflect(tool, roots, source, prelude='', probe=''):
    """The declarations of `source`, reflected after `prelude` and with `probe` (Slang text that
    follows it) when either is given: name -> records."""
    with tempfile.TemporaryDirectory(prefix='ludus-luslh-') as directory:
        staging = Path(directory)
        files = stage(roots, source, staging, INCLUDE.findall(prelude))
        root = posixpath.basename(source)
        if prelude or probe:
            (staging / PROBE).write_text(f'{prelude}#include "{root}"\n{probe}', encoding='utf-8')
            files, root = files + [PROBE], PROBE
        command = [str(tool), '--root', str(staging), '--source', root]
        for file in files:
            if file != root:
                command += ['--file', file]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise TemplateError(f'lusl-declarations could not run: {error}') from error
        if result.returncode != 0:
            raise TemplateError(f'lusl-declarations failed for {source}:\n{result.stderr.strip()}')
        return parse_records(result.stdout)


def parse_records(text):
    declarations = {}
    current = None
    for line in text.splitlines():
        fields = line.split('\t')
        if fields[0] in ('constant', 'struct', 'function', 'enum'):
            current = {'kind': fields[0], 'name': fields[1], 'type': fields[2] if len(fields) > 2 else '',
                       'value': fields[3] if len(fields) > 3 else None, 'members': []}
            declarations.setdefault(fields[1], []).append(current)
        elif fields[0] == 'field' and current is not None:
            current['members'].append({'name': fields[1], 'direction': '', 'type': fields[2]})
        elif fields[0] == 'parameter' and current is not None:
            current['members'].append({'name': fields[1], 'direction': fields[2], 'type': fields[3]})
        elif fields[0] == 'enumerator' and current is not None:
            current['members'].append({'name': fields[1], 'value': fields[2] if len(fields) > 2 else ''})
        else:
            raise TemplateError(f'unexpected lusl-declarations record: {line[:200]}')
    return declarations


def check_type(type_name, where):
    if type_name.startswith('?'):
        raise TemplateError(f'{where} has the Slang type {type_name[1:]}, which LUSL cannot declare yet; '
                            'remove it from the template or declare a LUSL-visible overload')
    if '[' in type_name:
        raise TemplateError(f'{where} has the array type {type_name}; LUSL arrays take a named size, so declare '
                            'it by hand in a separate extern block')
    return type_name


def spell(declaration):
    """The LUSL lines of one reflected declaration, indented for an extern block."""
    name = declaration['name']
    if declaration['kind'] == 'constant':
        return [f'{INDENT}static const {check_type(declaration["type"], name)} {name};']
    if declaration['kind'] == 'struct':
        lines = [f'{INDENT}struct {name}', f'{INDENT}{{']
        for field in declaration['members']:
            lines.append(f'{INDENT * 2}{check_type(field["type"], name + "." + field["name"])} {field["name"]};')
        return lines + [f'{INDENT}}};']
    if declaration['kind'] == 'enum':
        lines = [f'{INDENT}enum class {name} : {check_type(declaration["type"], name)}', f'{INDENT}{{']
        for enumerator in declaration['members']:
            if enumerator['value'] == '':
                raise TemplateError(f'{name}::{enumerator["name"]} has no value Slang can report')
            lines.append(f'{INDENT * 2}{enumerator["name"]} = {enumerator["value"]},')
        return lines + [f'{INDENT}}};']
    parameters = [f'{member["direction"]} {check_type(member["type"], name + "(" + member["name"] + ")")} '
                  f'{member["name"]}' for member in declaration['members']]
    head = f'{INDENT}{check_type(declaration["type"], name)} {name}('
    single = head + ', '.join(parameters) + ');'
    if len(single) <= COLUMN_LIMIT or len(parameters) < 2:
        return [single]
    alignment = ' ' * len(head)
    lines = [head + parameters[0] + ',']
    lines += [alignment + parameter + ',' for parameter in parameters[1:-1]]
    return lines + [alignment + parameters[-1] + ');']


def macro_values(template, tool, roots, prelude):
    """{(file, macro): value} for every @macro of `template`, one probe module per file."""
    wanted = {}
    for number, line in enumerate(template.read_text(encoding='utf-8').splitlines(), start=1):
        match = MACRO.match(line.strip())
        if match is not None:
            wanted.setdefault(match.group(4), {})[match.group(3)] = match.group(1)
    values = {}
    for file, macros in sorted(wanted.items()):
        names = sorted(macros)
        probe = ''.join(f'static const {SLANG_TYPES[macros[name]]} {PROBE_PREFIX}{index} = {name};\n'
                        for index, name in enumerate(names))
        declarations = reflect(tool, roots, file, prelude, probe)
        for index, name in enumerate(names):
            records = declarations.get(f'{PROBE_PREFIX}{index}', [])
            if not records or records[0]['value'] is None:
                raise TemplateError(f'{file} defines no integer macro {name}')
            values[(file, name)] = records[0]['value']
    return values


def expand(template, tool):
    """The header text that `template` (a `.luslh.in` path) generates."""
    root = template.parent
    text = template.read_text(encoding='utf-8')
    roots = [root]
    prelude = ''
    for line in text.splitlines():
        match = SEARCH.match(line.strip())
        if match is not None:
            roots.append((root / match.group(1)).resolve())
        if line.strip().startswith('@prelude '):
            prelude += line.strip()[len('@prelude '):] + '\n'
    macros = macro_values(template, tool, roots, prelude)
    lines = [f'// Generated from {template.name} by cmake/shaders/generate_luslh.py from Slang reflection;',
             '// edit the template, not this file.']
    declarations = None
    declared = set()
    for number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        where = f'{template.name}:{number}'
        if stripped.startswith('@search'):
            if SEARCH.match(stripped) is None:
                raise TemplateError(f'{where}: write @search "directory"')
        elif stripped.startswith('@prelude'):
            if declarations is not None or stripped == '@prelude':
                raise TemplateError(f'{where}: write @prelude <Slang text>, outside a block')
        elif stripped.startswith('@macro'):
            match = MACRO.match(stripped)
            if match is None or declarations is not None:
                raise TemplateError(f'{where}: write @macro int32|uint32 LUSL_NAME = MACRO from "file", outside a '
                                    'block')
            if match.group(2) == match.group(3):
                raise TemplateError(f'{where}: name the constant differently from the macro {match.group(3)}, which '
                                    'the generated Slang would expand')
            value = macros[(match.group(4), match.group(3))]
            lines.append(f'static const {match.group(1)} {match.group(2)} = {value};')
        elif stripped.startswith('@extern'):
            match = EXTERN.match(stripped)
            if match is None or declarations is not None:
                raise TemplateError(f'{where}: write @extern "file.slang" or @extern from "file.slang", outside a '
                                    'block')
            declarations = reflect(tool, roots, match.group(2), prelude)
            declared = set()
            lines += ['extern slang' if match.group(1) else f'extern slang "{match.group(2)}"', '{']
        elif stripped.startswith('@declare'):
            if declarations is None:
                raise TemplateError(f'{where}: @declare belongs inside an @extern block')
            for name in stripped.split()[1:]:
                if name not in declarations:
                    raise TemplateError(f'{where}: the Slang file declares no function, struct, enum or constant '
                                        f'{name}')
                if name in declared:
                    raise TemplateError(f'{where}: {name} is declared twice')
                declared.add(name)
                for declaration in declarations[name]:
                    try:
                        lines += spell(declaration)
                    except TemplateError as error:
                        raise TemplateError(f'{where}: {error}') from error
        elif stripped == '@end':
            if declarations is None:
                raise TemplateError(f'{where}: @end without @extern')
            declarations = None
            lines.append('};')
        elif stripped.startswith('@'):
            raise TemplateError(f'{where}: unknown directive {stripped.split()[0]}')
        elif declarations is not None:
            lines.append(INDENT + stripped if stripped else '')
        else:
            lines.append(line.rstrip())
    if declarations is not None:
        raise TemplateError(f'{template.name}: an @extern block has no @end')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tool', required=True, help='the lusl-declarations executable')
    parser.add_argument('--check', action='store_true', help='fail when a header differs from its template')
    parser.add_argument('templates', nargs='+', type=Path)
    args = parser.parse_args()
    stale = []
    for template in args.templates:
        if not template.name.endswith('.luslh.in'):
            print(f'generate_luslh: {template} is not a .luslh.in template', file=sys.stderr)
            return 2
        try:
            text = expand(template.resolve(), args.tool)
        except TemplateError as error:
            print(f'generate_luslh: {error}', file=sys.stderr)
            return 1
        header = template.with_suffix('')
        current = header.read_text(encoding='utf-8') if header.is_file() else None
        if current == text:
            continue
        if args.check:
            stale.append(header)
        else:
            header.write_text(text, encoding='utf-8')
    if stale:
        names = ' '.join(str(path) for path in stale)
        print(f'generate_luslh: stale generated headers: {names}\n'
              'Regenerate them with: python3 cmake/shaders/generate_luslh.py --tool <build>/tools/lusl-declarations/'
              'lusl-declarations ' + ' '.join(str(template) for template in args.templates), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
