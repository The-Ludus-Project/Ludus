"""Generate LUSL's preprocessor view of the shared C++/Slang layout macros."""

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RENDERER_SHADERS = Path('modules/graphics/renderer/shaders')
RENDERER_HEADERS = Path('modules/graphics/renderer/include/ludus/graphics/renderer')
GEOMETRY_HEADERS = Path('modules/foundation/geometry/include/ludus/foundation/geometry/detail')
OWNERS = (
    (RENDERER_SHADERS / 'renderer_gpu_preview_layout.h', ()),
    (RENDERER_SHADERS / 'renderer_reference_layout.h', ('renderer_gpu_preview_layout.luslh',)),
    (RENDERER_HEADERS / 'scene_shader_layout.h', ()),
    (GEOMETRY_HEADERS / 'point_search_layout.h', ()),
)
# The preview owner includes these for its C++ and Slang consumers. Its layout
# macros do not use their declarations.
PREVIEW_DECLARATIONS = re.compile(
    r'#ifdef __cplusplus\n'
    r'# *include <ludus/content/scene_format/material\.h>\n'
    r'# *include <ludus/graphics/renderer/lighting_layout\.h>\n'
    r'#else\n# *include "lighting_layout\.h"\n# *include "material\.h"\n#endif\n'
)
# Native Slang variants can override their workgroup shape. The generated LUSL
# view publishes the owner's defaults.
PREVIEW_DEFAULT = re.compile(
    r'#ifndef (LUDUS_GPU_PREVIEW_TRANSPORT_GROUP_(?:WIDTH|HEIGHT))\n'
    r'# *define \1 ([0-9]+)\n#endif\n'
)
UNSIGNED_SUFFIX = re.compile(r"\b(0[xX][0-9a-fA-F]+|[0-9]+)[uU]\b")
DEFINE = re.compile(r'#define (LUDUS_[A-Z_0-9]+)[ \t]+(\S.*)')


def render(source, owner, includes):
    source = re.sub(r'\\\n[ \t]*', ' ', source)
    if owner.name == 'renderer_gpu_preview_layout.h':
        source = PREVIEW_DECLARATIONS.sub('', source)
        source = PREVIEW_DEFAULT.sub(r'#define \1 \2\n', source)

    lines = [f'// Generated from {owner.name} by generate_lusl_layouts.py. Edit the owner.',
             '#pragma once', '']
    if includes:
        lines.extend(f'#include "{name}"' for name in includes)
        lines.append('')
    for line in source.splitlines():
        line = line.partition('//')[0].strip()
        if line in ('', '#pragma once'):
            continue
        match = DEFINE.fullmatch(line)
        if match is None:
            raise ValueError(f'{owner}: expected a shared layout macro, got {line!r}')
        name, expression = match.groups()
        expression = UNSIGNED_SUFFIX.sub(r"\1", expression)
        lines.append(f'#define {name} {" ".join(expression.split())}')
    return '\n'.join([*lines, ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    stale = []
    for owner, includes in OWNERS:
        path = args.root / owner
        output = path.with_suffix('.luslh')
        try:
            expected = render(path.read_text(encoding='utf-8'), owner, includes).encode()
            if args.check:
                if not output.is_file() or output.read_bytes() != expected:
                    stale.append(str(output))
            else:
                output.write_bytes(expected)
        except (OSError, ValueError) as error:
            parser.exit(1, f'{error}\n')
    if stale:
        parser.exit(1, 'Stale LUSL layout headers:\n  ' + '\n  '.join(stale)
                    + '\nRun cmake --build <build> --target ludus_generate_lusl_layouts or '
                    'python3 cmake/shaders/generate_lusl_layouts.py to regenerate them.\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
