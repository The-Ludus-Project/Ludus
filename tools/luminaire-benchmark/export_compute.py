#!/usr/bin/env python3
"""Bake a portable flow into a self-contained Ludus comparison graph.

Weights remain immutable in the generated package. Shader chunks obey the
existing package limits; no runtime buffer or engine admission limit is raised.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from benchmark import Panel, TEMPLATE
from portable_flow import HIDDEN, LAYERS, RADIUS, SHAPES, PortableFlow

CHUNK_WORDS = 8192
SHADERS = Path(__file__).parent / 'shaders'


def emit_array(name, values, spelling='uint'):
    values = list(values)
    count = name + '_COUNT'
    literals = [f'0x{int(value):08x}u' for value in values]
    rows = ['    ' + ', '.join(literals[index:index+8]) + ',' for index in range(0, len(literals), 8)]
    return f'#pragma once\nstatic const uint {count} = {len(values)};\nstatic const {spelling} {name}[{count}] = {{\n' + '\n'.join(rows) + '\n};\n'


def export(model_path, room, destination):
    panel = Panel()
    model = PortableFlow(model_path, panel)
    identity = json.loads((room / 'comparison.json').read_text())
    if identity['fixture_sha256'] != panel.identity():
        raise ValueError('Room and trained model have different radiance identities.')
    if (room / 'graph/main_shaders/luminaire.slang').read_bytes() != (TEMPLATE / 'main_shaders/luminaire.slang').read_bytes():
        raise ValueError('Room emission source was edited. Prepare a fresh room and retrain.')
    if destination.exists():
        raise FileExistsError('Use a new graph output directory to preserve previous captures.')
    values = list(model.grid.astype(np.float32).ravel())
    offsets = []
    for coupling in range(LAYERS):
        for layer in range(len(SHAPES)):
            offsets.append(len(values))
            values.extend(model.arrays[f'w{coupling}_{layer}'].ravel())
            values.extend(model.arrays[f'b{coupling}_{layer}'].ravel())
    # Per-layer signed-byte storage keeps the generated artifact within the
    # engine's existing bounds. The CPU archive contains the exact decoded model.
    segments = [0] + offsets + [len(values)]
    scales = []
    quantized_values = np.zeros(len(values), dtype=np.float32)
    packed_values = np.zeros((len(values)+3)//4*4, dtype=np.uint8)
    for begin, end in zip(segments[:-1], segments[1:]):
        original = np.asarray(values[begin:end], dtype=np.float32)
        scale = np.float32(max(float(np.max(np.abs(original))), 1e-20) / 127)
        integer = np.clip(np.rint(original / scale), -127, 127).astype(np.int32)
        quantized_values[begin:end] = integer.astype(np.float32) * scale
        packed_values[begin:end] = (integer + 128).astype(np.uint8)
        scales.append(scale)
    bits = packed_values.view('<u4')
    if not np.all(np.isfinite(np.asarray(values))):
        raise ValueError('Model contains nonfinite weights; training must be repaired before export.')
    parts = []
    branches = []
    for chunk, begin in enumerate(range(0, len(bits), CHUNK_WORDS)):
        name = f'PURELIGHT_WORDS_{chunk}'
        filename = f'purelight_weights_{chunk}.slang'
        text = emit_array(name, bits[begin:begin+CHUNK_WORDS])
        if len(text.encode()) > 128 * 1024:
            raise ValueError('Weight chunk exceeds the engine source-file budget.')
        parts.append((filename, text))
        branches.append(f'    case {chunk}: word = {name}[wordIndex % PURELIGHT_WORD_CHUNK_COUNT]; break;')
    if len(parts) > 64:
        raise ValueError('Model exceeds this exporter\'s bounded shader closure. Use a buffer-backed extension.')
    shutil.copytree(room / 'graph', destination)
    shader_root = destination / 'main_shaders'
    header = '#pragma once\n// Generated immutable FP32 model; see comparison-model.json for identity and training.\n'
    for filename, text in parts:
        (shader_root / filename).write_text(text)
        header += f'#include "{filename}"\n'
    header += f'static const uint PURELIGHT_WORD_CHUNK_COUNT = {CHUNK_WORDS};\n'
    header += f'static const uint PURELIGHT_GRID_NODE_COUNT = {len(model.keys)};\n'
    header += f'static const float PURELIGHT_RADIUS_WORLD = {RADIUS:.9g};\n'
    header += emit_array('PURELIGHT_GRID_KEYS', model.keys)
    header += emit_array('PURELIGHT_NETWORK_OFFSETS', offsets)
    for name, value in [('RADIANCE_SCALE', 6), ('BEAM_WIDTH', .02), ('BEAM_STEERING', .12), ('BEAM_SPREAD', .08), ('BEAM_TWIST', .04)]:
        header += f'static const float PURELIGHT_PROFILE_{name} = {value};\n'
    header += emit_array('PURELIGHT_SCALE_OFFSETS', segments[:-1])
    header += emit_array('PURELIGHT_SCALE_BITS', np.asarray(scales, dtype='<f4').view('<u4'))
    header += 'static const uint PURELIGHT_BYTE_BITS = 8;\nstatic const uint PURELIGHT_BYTE_MASK = 255;\nstatic const int PURELIGHT_BYTE_ZERO = 128;\nstatic const uint PURELIGHT_VALUES_PER_WORD = 4;\nfloat GetPureLightModelValue(uint index)\n{\n    uint wordIndex = index / PURELIGHT_VALUES_PER_WORD;\n    uint word = 0;\n    switch (wordIndex / PURELIGHT_WORD_CHUNK_COUNT)\n    {\n'
    header += '\n'.join(branches) + '\n    default: break;\n    }\n    uint segment = 0;\n    [loop] for (uint candidate = 1; candidate < PURELIGHT_SCALE_OFFSETS_COUNT; ++candidate)\n    {\n        if (index >= PURELIGHT_SCALE_OFFSETS[candidate])\n            segment = candidate;\n    }\n    int value = int((word >> ((index % PURELIGHT_VALUES_PER_WORD) * PURELIGHT_BYTE_BITS)) & PURELIGHT_BYTE_MASK) - PURELIGHT_BYTE_ZERO;\n    return float(value) * asfloat(PURELIGHT_SCALE_BITS[segment]);\n}\n'
    (shader_root / 'purelight_weights.slang').write_text(header)
    shutil.copyfile(SHADERS / 'portable_flow.slang', shader_root / 'portable_flow.slang')
    shutil.copyfile(Path(__file__).parent / 'NOTICE.txt', destination / 'PureLight-NOTICE.txt')
    shade_path = shader_root / 'shade.slang'
    shade = shade_path.read_text().replace('#include "slice_sampling.slang"', '#include "slice_sampling.slang"\n#include "portable_flow.slang"')
    old = 'light = SampleLuminaireArea(luminaire, receiver, areaSample);'
    if shade.count(old) != 1:
        raise ValueError('The source shading integration changed; update the exporter before using it.')
    shade = shade.replace(old, 'light = SamplePortablePureLight(luminaire, receiver, seedSample);')
    shade = shade.replace('ShadeUniformArea', 'ShadePortablePureLight').replace('UniformArea', 'PortablePureLight')
    begin = shade.index('// Passes 1 to 3')
    end = shade.index("/// The template's view of a scene panel luminaire")
    shade = shade[:begin] + '// Generated comparison: defensive slice, raw slice and portable PureLight receiver flow.\n// All three use the unchanged analytic luminaire radiance, BSDF and visibility implementation.\n// The World luminaire places the panel; author it as the room places it (benchmark.py CENTER).\n// The emission profile is frozen to the trained model; retrain and re-export after edits.\n\n' + shade[end:]
    for field, setting in (('RadianceScale', 'RADIANCE_SCALE'), ('BeamWidth', 'BEAM_WIDTH'),
                           ('BeamSteering', 'BEAM_STEERING'), ('BeamSpread', 'BEAM_SPREAD'), ('BeamTwist', 'BEAM_TWIST')):
        assignment = f'luminaire.{field} = info.{field};'
        if shade.count(assignment) != 1:
            raise ValueError('The source luminaire integration changed; update the exporter before using it.')
        shade = shade.replace(assignment, f'luminaire.{field} = PURELIGHT_PROFILE_{setting};')
    # This controlled room has unit Lambertian receivers. Specialize the
    # shared physical evaluator equally for every proposal; general material
    # transport is outside this benchmark's declared integral.
    begin = shade.index('    float3 bsdfValue;')
    end = shade.index('    if (cosSurface <= 0', begin)
    shade = shade[:begin] + '    float3 bsdfValue = float3(1 / PI);\n' + shade[end:]
    shade_path.write_text(shade)
    # The third output becomes the learned flow; the original room graph retains
    # its independent uniform-area output for control experiments.
    for filename in ('main.lushader.json', 'main.lurender.json', 'library.lurender.json'):
        path = destination / filename
        text = path.read_text().replace('ShadeUniformArea', 'ShadePortablePureLight').replace('shadeUniformArea', 'shadePortablePureLight')
        text = text.replace('UniformArea', 'PortablePureLight').replace('uniformArea', 'portablePureLight')
        document = json.loads(text)
        if filename == 'main.lushader.json':
            document['settings'] = [setting for setting in document['settings'] if not setting['id'].startswith('LUMINAIRE_')]
        path.write_text(json.dumps(document, indent=2)+'\n')
    quantized = dict(model.arrays)
    quantized['grid_features'] = quantized_values[:offsets[0]].reshape(model.grid.shape)
    for item, (begin, end) in enumerate(zip(offsets, offsets[1:] + [len(values)])):
        coupling, layer = divmod(item, len(SHAPES))
        shape = SHAPES[layer]
        count = int(np.prod(shape))
        quantized[f'w{coupling}_{layer}'] = quantized_values[begin:begin+count].reshape(shape)
        quantized[f'b{coupling}_{layer}'] = quantized_values[begin+count:end]
    quantized['metadata'] = np.array(json.dumps(dict(model.metadata, storage='per-layer int8; evaluation fp32')))
    np.savez(destination / 'portable-model.npz', **quantized)
    metadata = dict(model.metadata, storage='per-layer int8; evaluation fp32', model_sha256=hashlib.sha256(model_path.read_bytes()).hexdigest(),
                    model_words=len(bits), shader_chunks=len(parts),
                    output='portablePureLightOutput', profile_editing='frozen; retrain and re-export to edit emission',
                    scope='portable receiver-flow adaptation; original analytic radiance; no CUDA inference')
    (destination / 'comparison-model.json').write_text(json.dumps(metadata, indent=2)+'\n')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--room', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(export(args.model, args.room, args.output), indent=2))


if __name__ == '__main__':
    main()
