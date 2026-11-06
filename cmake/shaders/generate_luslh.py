"""Generate the SDK's LUSL `.luslh` headers from Slang reflection.

Each header has a template, `<name>.luslh.in`, beside it. The template is the header with its
`extern slang` blocks written as directives:

    @extern "math.slang"          a block that names math.slang (lowering includes it)
    @extern from "ray_shader.slang"   a path-less block; ray_shader.slang is only reflected
    /// One addressed draw of the counter-based random generator.
    @declare RandomAddress        a struct, constant or function (every overload) of that file
    @declare PI ComputeLuminance  several names in order
    @end                          closes the block

Other lines, including doc comments before a `@declare`, are copied. Declarations are spelled
from what `lusl-declarations` reflects, so a header cannot drift from the Slang it declares
(docs/modules/tooling/shader-language.md#sdk-declarations). `--check` reports stale headers;
without it the headers are rewritten.
"""
import argparse
import posixpath
import re
import subprocess
import sys
from pathlib import Path

TIMEOUT_SECONDS = 120
# The repository's .clang-format ColumnLimit; longer signatures wrap one parameter per line.
COLUMN_LIMIT = 120
INDENT = '    '
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"\n]+)"', re.MULTILINE)
EXTERN = re.compile(r'@extern(?:[ \t]+(from))?[ \t]+"([^"\n]+)"[ \t]*\Z')


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


def reflect(tool, root, source):
    """The declarations of `source`: name -> list of records, in Slang's order."""
    files = collect(root, source)
    command = [str(tool), '--root', str(root), '--source', source]
    for file in files[1:]:
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
        if fields[0] in ('constant', 'struct', 'function'):
            current = {'kind': fields[0], 'name': fields[1], 'type': fields[2] if len(fields) > 2 else '',
                       'members': []}
            declarations.setdefault(fields[1], []).append(current)
        elif fields[0] == 'field' and current is not None:
            current['members'].append({'name': fields[1], 'direction': '', 'type': fields[2]})
        elif fields[0] == 'parameter' and current is not None:
            current['members'].append({'name': fields[1], 'direction': fields[2], 'type': fields[3]})
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


def expand(template, tool):
    """The header text that `template` (a `.luslh.in` path) generates."""
    root = template.parent
    lines = [f'// Generated from {template.name} by cmake/shaders/generate_luslh.py from Slang reflection;',
             '// edit the template, not this file.']
    declarations = None
    declared = set()
    for number, line in enumerate(template.read_text(encoding='utf-8').splitlines(), start=1):
        stripped = line.strip()
        where = f'{template.name}:{number}'
        if stripped.startswith('@extern'):
            match = EXTERN.match(stripped)
            if match is None or declarations is not None:
                raise TemplateError(f'{where}: write @extern "file.slang" or @extern from "file.slang", outside a block')
            declarations = reflect(tool, root, match.group(2))
            declared = set()
            lines += ['extern slang' if match.group(1) else f'extern slang "{match.group(2)}"', '{']
        elif stripped.startswith('@declare'):
            if declarations is None:
                raise TemplateError(f'{where}: @declare belongs inside an @extern block')
            for name in stripped.split()[1:]:
                if name not in declarations:
                    raise TemplateError(f'{where}: the Slang file declares no function, struct or constant {name}')
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
        if template.suffix != '.in' or not template.name.endswith('.luslh.in'):
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
