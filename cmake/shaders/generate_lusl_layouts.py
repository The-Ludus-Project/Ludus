"""Generate LUSL's preprocessor view of the shared C++/Slang layout macros."""

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RENDERER_SHADERS = Path('modules/graphics/renderer/shaders')
RENDERER_HEADERS = Path('modules/graphics/renderer/include/ludus/graphics/renderer')
GEOMETRY_HEADERS = Path('modules/foundation/geometry/include/ludus/foundation/geometry/detail')
RENDER_GRAPH_HEADERS = Path('modules/content/render_graph_format/include/ludus/content/render_graph_format/detail')
OWNERS = (
    (RENDERER_SHADERS / 'renderer_gpu_preview_layout.h', ('scene_shader_layout.luslh',)),
    (RENDERER_SHADERS / 'renderer_reference_layout.h', ('renderer_gpu_preview_layout.luslh',)),
    (RENDERER_HEADERS / 'scene_shader_layout.h', ()),
    (GEOMETRY_HEADERS / 'point_search_layout.h', ()),
    (RENDERER_HEADERS / 'detail' / 'deferred_binding_slots.inc', ()),
    (RENDERER_HEADERS / 'deferred_layout.h', ('deferred_binding_slots.luslh',)),
    (RENDERER_SHADERS / 'skin_diffusion_layout.h', ()),
    (RENDER_GRAPH_HEADERS / 'light_transformer_layout.h', ()),
    (RENDER_GRAPH_HEADERS / 'light_transformer_encoder_layout.h', ()),
    (RENDERER_HEADERS / 'lighting_layout.h', ()),
    (RENDERER_SHADERS / 'renderer_atmosphere_layout.h', ()),
    (RENDERER_SHADERS / 'renderer_reference_sky_layout.h', ()),
    (RENDERER_SHADERS / 'ggx_albedo_table_layout.h', ()),
    (RENDERER_SHADERS / 'wave_connection_layout.h', ()),
)
# The preview owner includes the scene failure reason count for C++ and Slang.
# The generated view includes its LUSL counterpart through OWNERS.
PREVIEW_DECLARATIONS = re.compile(
    r'#ifdef __cplusplus\n'
    r'# *include <ludus/content/scene_format/material\.h>\n'
    r'# *include <ludus/graphics/renderer/lighting_layout\.h>\n'
    r'# *include <ludus/graphics/renderer/scene_shader_layout\.h>\n'
    r'#else\n'
    r'# *include "lighting_layout\.h"\n'
    r'# *include "material\.h"\n'
    r'# *include "scene_shader_layout\.h"\n'
    r'#endif\n'
)
# Native Slang variants can override their workgroup shape. The generated LUSL
# view publishes the owner's defaults.
PREVIEW_DEFAULT = re.compile(
    r'#ifndef (LUDUS_GPU_PREVIEW_TRANSPORT_GROUP_(?:WIDTH|HEIGHT))\n'
    r'# *define \1 ([0-9]+)\n#endif\n'
)
# The deferred owner selects its binding-slot include, opens its C++ namespace and checks the
# authored binding count under __cplusplus. The generated view includes deferred_binding_slots.luslh
# through OWNERS; its constants use only that header and each other. The lighting owner's
# __cplusplus blocks choose LightingLayoutValue and open and close its namespace.
CPLUSPLUS_BLOCK = re.compile(r'#ifdef __cplusplus\n.*?#endif\n', re.S)
LIGHTING_INCLUDE = '#include "lighting_layout.h"\n'
# A shared constant, `static const LightingLayoutValue NAME = expression;`, possibly over several lines.
CONSTANT = re.compile(r'static const LightingLayoutValue ([A-Z_0-9]+) = (\S.*);')
UNSIGNED_SUFFIX = re.compile(r"\b(0[xX][0-9a-fA-F]+|[0-9]+)[uU]\b")
# LUSL literals take no suffix; C and Slang spell a binary32 literal with f, as in 1e-6f.
FLOAT_SUFFIX = re.compile(r"\b([0-9]+\.[0-9]*(?:[eE][-+]?[0-9]+)?|[0-9]+[eE][-+]?[0-9]+)[fF]\b")
# An object-like macro, or a function-like one such as LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(inputCount, outputCount).
DEFINE = re.compile(r'#define (LUDUS_[A-Z_0-9]+(?:\([A-Za-z_][A-Za-z_0-9]*(?:, *[A-Za-z_][A-Za-z_0-9]*)*\))?)[ \t]+(\S.*)')


def render(source, owner, includes):
    source = re.sub(r'\\\n[ \t]*', ' ', source)
    if owner.name == 'renderer_gpu_preview_layout.h':
        source = PREVIEW_DECLARATIONS.sub('', source)
        source = PREVIEW_DEFAULT.sub(r'#define \1 \2\n', source)
    if owner.name == 'deferred_layout.h':
        source = CPLUSPLUS_BLOCK.sub('', source.replace(LIGHTING_INCLUDE, ''))
    if owner.name == 'lighting_layout.h':
        source = CPLUSPLUS_BLOCK.sub('', source)

    lines = [f'// Generated from {owner.name} by generate_lusl_layouts.py. Edit the owner.',
             '#pragma once', '']
    if includes:
        lines.extend(f'#include "{name}"' for name in includes)
        lines.append('')
    statement = ''
    for line in source.splitlines():
        line = line.partition('//')[0].strip()
        if line in ('', '#pragma once'):
            continue
        if statement != '' or line.startswith('static const '):
            statement = f'{statement} {line}'.strip()
            if not statement.endswith(';'):
                continue
            match = CONSTANT.fullmatch(statement)
            statement = ''
            if match is None:
                raise ValueError(f'{owner}: expected a shared layout constant, got {line!r}')
            name, expression = match.groups()
            lines.append(f'static const uint32 {name} = {" ".join(expression.split())};')
            continue
        match = DEFINE.fullmatch(line)
        if match is None:
            raise ValueError(f'{owner}: expected a shared layout macro, got {line!r}')
        name, expression = match.groups()
        expression = FLOAT_SUFFIX.sub(r"\1", UNSIGNED_SUFFIX.sub(r"\1", expression))
        lines.append(f'#define {name} {" ".join(expression.split())}')
    if statement != '':
        raise ValueError(f'{owner}: unterminated shared layout constant {statement!r}')
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
