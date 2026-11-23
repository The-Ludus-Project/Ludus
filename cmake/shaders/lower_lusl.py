"""Lower a standalone LUSL shader for ludus_compile_shader.

luslc lowers SOURCE to one Slang module, which compile_shader.py then compiles like an authored
Slang source. The LUSL compiler assigns binding slots and vertex input locations
(docs/architecture/shading-language.md#resources-and-bindings); this script writes them as named
C++ constants in <name>_slots.h, so host code never spells a slot number. It also writes a
depfile of every file the lowering may read, found by the same lexical scan luslc uses.

compile_shader.py defines LUDUS_GLSL_ES=1 only for the GLSL ES (WebGL 2) artifact. A LUSL
preprocessor reads that macro before Slang sees it, so a source that tests it is lowered once with
LUDUS_GLSL_ES=0 and once with =1; the module selects the lowering by the macro's value. Both
lowerings must assign the same slots and locations, because host code binds one slot table.
See docs/modules/tooling/shader-language.md, "Standalone shaders".
"""
import argparse
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 120
# Paths LUSL names: `#include "x"` and `extern slang "x"`. The scan is lexical and conservative.
NAMED_PATH = re.compile(r'(?:#[ \t]*include|\bextern[ \t\r\n]+slang)[ \t\r\n]*"([^"\n]+)"')
LANGUAGE_FILES = ('.lusl', '.luslh')
# Target macros that compile_shader.py defines for one artifact only (its GLSL ES variant).
GLSL_ES_MACRO = 'LUDUS_GLSL_ES'
IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_.]*\Z')


def fail(message):
    print(f'ludus_compile_shader: {message}', file=sys.stderr)
    raise SystemExit(1)


def stage_snapshot(root, source, include_directories, snapshot):
    """Mount the source directory and each external include root in one private luslc snapshot.

    Preserve the original source-root fallback and caller-supplied search order, even when
    the SDK and project are on different drives. Only copy files named by the shader.
    """
    root = root.resolve()
    namespace = '_ludus_shared'
    while (root / namespace).exists():
        namespace += '_'
    mounts = {'.': root}
    directories = []
    for include in include_directories:
        directory = (root / include).resolve()
        if directory == root:
            continue
        if directory.is_relative_to(root):
            logical = directory.relative_to(root).as_posix()
        else:
            logical = f'{namespace}/{len(mounts)}'
            mounts[logical] = directory
        directories.append(logical)

    files = {}
    pending = [source]
    while pending:
        logical = posixpath.normpath(pending.pop())
        if logical in files or logical in ('.', '..') or logical.startswith(('../', '/')):
            continue
        mount = '.'
        if logical.startswith(namespace + '/'):
            mount = '/'.join(logical.split('/')[:2])
            if mount not in mounts or logical == mount:
                continue
        physical = (mounts[mount] / posixpath.relpath(logical, mount)).resolve()
        if not physical.is_relative_to(mounts[mount]) or not physical.is_file():
            continue
        files[logical] = physical
        staged = snapshot / logical
        staged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(physical, staged)
        if logical.endswith(LANGUAGE_FILES):
            for name in NAMED_PATH.findall(staged.read_text(encoding='utf-8', errors='replace')):
                if not posixpath.isabs(name) and not re.search(r'[:\\\x00-\x1f\x7f]', name):
                    pending += [posixpath.join(posixpath.dirname(logical), name), name]
                    pending += [posixpath.join(include, name) for include in directories]
    return directories, files


def remap_slang_includes(generated, files, output_directory):
    """Point emitted extern Slang includes at the original files, beyond the temp snapshot."""
    def remap(match):
        physical = files[posixpath.normpath(match[1])]
        try:
            relative = os.path.relpath(physical, output_directory)
        except ValueError:  # Windows paths on different drives.
            relative = physical.as_posix()
        return f'#include "{Path(relative).as_posix()}"'

    return re.sub(r'^#include "([^"\n]+)"$', remap, generated, flags=re.MULTILINE)


def constant_name(name):
    """`imageSampler` -> `IMAGE_SAMPLER`, `input.PositionUv` -> `INPUT_POSITION_UV`."""
    words = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', name.replace('.', '_'))
    return re.sub(r'_+', '_', words).upper()


def slot_header(name, report):
    """C++ constants for the slot table and vertex input locations in `report`."""
    slots, locations = {}, {}
    for line in report.splitlines():
        fields = line.split('\t')
        if fields[0] == 'binding' and len(fields) in (5, 6):
            slots[constant_name(fields[2])] = (int(fields[4]), fields[2], fields[3], int(fields[5]) if len(fields) == 6 else 1)
        elif fields[0] == 'vertex-input' and len(fields) == 4:
            constant = constant_name(fields[2])
            location = int(fields[3])
            previous = locations.setdefault(constant, (location, fields[2]))
            if previous[0] != location:
                fail(f"two vertex entry points give '{fields[2]}' the locations {previous[0]} and {location}; "
                     'give each entry point its own input names')
    lines = ['// Generated by ludus_compile_shader from the LUSL compiler\'s slot table; do not edit.',
             '#pragma once', '', '#include <ludus/foundation/base/types.h>', '',
             f'/// Binding slots and vertex input locations of the LUSL shader {name}, assigned in',
             '/// declaration order (docs/architecture/shading-language.md#resources-and-bindings).',
             f'namespace ludus::shaders::{name}::slots', '{']
    for constant, (slot, variable, kind, count) in sorted(slots.items(), key=lambda item: item[1][0]):
        span = f' ({count} descriptors)' if count > 1 else ''
        lines += [f'/// Group-zero slot of the {kind} `{variable}`{span}.',
                  f'inline constexpr foundation::uint32 {constant} = {slot};']
    lines += ['}', '', f'namespace ludus::shaders::{name}::locations', '{']
    for constant, (location, attribute) in sorted(locations.items(), key=lambda item: item[1][0]):
        lines += [f'/// Vertex input location of `{attribute}`.',
                  f'inline constexpr foundation::uint32 {constant} = {location};']
    lines += ['}', '']
    return '\n'.join(lines)


def publish(path, text):
    """Replace `path` atomically, so a failed build never leaves a partial file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(text, encoding='utf-8')
    os.replace(temporary, path)


def depfile_path(path):
    return path.as_posix().replace('$', '$$').replace('#', r'\#').replace(' ', r'\ ')


def references_macro(files, macro):
    """Whether any LUSL file of the snapshot names `macro`, by the same lexical scan as includes."""
    pattern = re.compile(rf'\b{re.escape(macro)}\b')
    return any(pattern.search(path.read_text(encoding='utf-8', errors='replace')) is not None
               for logical, path in files.items() if logical.endswith(LANGUAGE_FILES))


def combine_variants(default, variant, macro):
    """One module holding both lowerings; Slang's preprocessor selects one by `macro`."""
    def ending(text):
        return text if text.endswith('\n') else text + '\n'
    # Slang's preprocessor evaluates both `&&` operands and warns about an undefined macro, so the
    # selector is a generated macro that is always defined (`_LUSL` names are reserved for lowering).
    selector = f'_LUSL_SELECT_{macro}'
    return (f'// Lowered with {macro}=0 and {macro}=1; ludus_compile_shader defines it for one artifact.\n'
            f'#if defined({macro})\n#define {selector} {macro}\n#else\n#define {selector} 0\n#endif\n'
            f'#if {selector}\n{ending(variant)}#else\n{ending(default)}#endif\n')


def lower(luslc, snapshot, directories, source, style, defines, directory):
    """Run `luslc lower` once; return the generated Slang text and the slot report."""
    report = Path(directory) / 'report.txt'
    output = Path(directory) / 'generated.slang'
    command = [luslc, 'lower', '--root', str(snapshot), '--style', style,
               '--output', str(output), '--report', str(report)]
    for include in directories:
        command += ['--include-directory', include]
    for define in defines:
        command += ['--define', define]
    command.append(source)
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        fail(f'luslc could not run: {error}')
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        raise SystemExit(1)
    return output.read_text(encoding='utf-8'), report.read_text(encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('luslc', 'root', 'source', 'output', 'slots', 'depfile', 'name'):
        parser.add_argument('--' + key, required=True)
    parser.add_argument('--include-directory', action='append', default=[])
    parser.add_argument('--style', default='strict', choices=('strict', 'warn', 'off'))
    args = parser.parse_args()
    if IDENTIFIER.match(args.name) is None:
        fail(f"'{args.name}' is not a shader name")
    root = Path(args.root)
    with tempfile.TemporaryDirectory(prefix='ludus-lusl-') as directory:
        snapshot = Path(directory) / 'snapshot'
        directories, files = stage_snapshot(root, args.source, args.include_directory, snapshot)
        variants = [[]]
        if references_macro(files, GLSL_ES_MACRO):
            variants = [[f'{GLSL_ES_MACRO}=0'], [f'{GLSL_ES_MACRO}=1']]
        lowered = []
        for index, defines in enumerate(variants):
            work = Path(directory) / f'variant{index}'
            work.mkdir()
            text, report = lower(args.luslc, snapshot, directories, args.source, args.style, defines, work)
            lowered.append((text, slot_header(args.name, report)))
        generated, slots = lowered[0]
        if len(lowered) > 1:
            if lowered[1][1] != slots:
                fail(f'{args.source}: {GLSL_ES_MACRO} changes the binding slots or vertex input locations; '
                     f'keep resource and input declarations outside #if {GLSL_ES_MACRO}')
            generated = combine_variants(generated, lowered[1][0], GLSL_ES_MACRO)
        generated = remap_slang_includes(generated, files, Path(args.output).parent)
    publish(Path(args.slots), slots)
    inputs = ' '.join(depfile_path(path) for path in sorted(set(files.values())))
    # Ninja reads one target per depfile: the generated module, written last.
    publish(Path(args.depfile), depfile_path(Path(args.output)) + f': {inputs}\n')
    publish(Path(args.output), generated)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
