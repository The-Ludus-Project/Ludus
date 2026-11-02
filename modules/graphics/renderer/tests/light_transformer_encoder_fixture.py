"""Generate deterministic, redistributable encoder parameters without ML dependencies.

Only synthetic parameters are generated. The optional oracle refresh uses CPU
PyTorch; released checkpoints remain external qualification inputs.
"""

import argparse
import array
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'scripts/python'))
from ludus_tools.light_transformer_encoder import (encoder_schema, ENCODER_LAYOUT, MAGIC, VERSION,
                                                  HEADER_WORDS, RECORD_WORDS, NORMALIZE, CONVOLUTION, POOL)


def layer_size(specification):
    kind, name, inputs, outputs, kernel, normalization, convolution = specification
    return outputs * 2 if kind == NORMALIZE else inputs * outputs * kernel ** 3 + outputs * (3 if kind == POOL else 1)


def make_layer(specification):
    kind, name, inputs, outputs, kernel, normalization, convolution = specification
    values = array.array('f', [0]) * layer_size(specification)
    if kind == NORMALIZE:
        for channel in range(outputs):
            values[channel] = 0.9 + (channel % 7) * 0.03
            values[outputs + channel] = (channel % 5 - 2) * 0.007
    else:
        volume = kernel ** 3
        center = volume // 2
        bias = inputs * outputs * volume
        for channel in range(outputs):
            row = channel * inputs * volume
            values[row + center * inputs + channel % inputs] = 0.15 + (channel % 7) * 0.01
            values[row + center * inputs + (channel * 5 + 1) % inputs] -= 0.04
            if kind == CONVOLUTION:
                values[row + (center - 1) * inputs + (channel + 1) % inputs] = 0.023
            values[bias + channel] = (channel % 11 - 5) * 0.002
            if kind == POOL:
                # Negative BN gamma is a regression for max-before-normalization.
                values[bias + outputs + channel] = -0.7 if channel % 9 == 0 else 1.1
                values[bias + 2 * outputs + channel] = (channel % 5 - 2) * 0.005
    return values


def write_encoder(path):
    schema = list(encoder_schema())
    records = []
    cursor = 0
    for kind, name, inputs, outputs, kernel, normalization, convolution in schema:
        size = layer_size((kind, name, inputs, outputs, kernel, normalization, convolution))
        records.append((kind, inputs, outputs, kernel, cursor, size, 0, 0))
        cursor += size
    assert len(schema) == ENCODER_LAYOUT['LAYER_COUNT']
    assert cursor == ENCODER_LAYOUT['WEIGHT_COUNT']
    with Path(path).open('wb') as stream:
        stream.write(struct.pack('<8I', MAGIC, VERSION, len(records), cursor, 0, 0, 0, 0))
        for record in records:
            stream.write(struct.pack('<8I', *record))
        for specification in schema:
            values = make_layer(specification)
            if sys.byteorder != 'little':
                values.byteswap()
            values.tofile(stream)


def make_state(torch):
    """Reconstruct equivalent ordinary PyTorch affine/BN layers for the CPU oracle."""
    state = {}
    for specification in encoder_schema():
        kind, name, inputs, outputs, kernel, normalization, convolution = specification
        values = torch.tensor(make_layer(specification), dtype=torch.float32)
        if kind == NORMALIZE:
            state[name + '.weight'] = values[:outputs]
            state[name + '.bias'] = values[outputs:]
            continue
        weights = inputs * outputs * kernel ** 3
        shape = ((outputs, kernel, kernel, kernel, inputs) if kind == CONVOLUTION else
                 (outputs, inputs, 1) if convolution else (outputs, inputs))
        state[name + '.weight'] = values[:weights].reshape(shape)
        state[name + '.bias'] = values[weights:weights + outputs]
        if normalization:
            epsilon = 1e-3 if name.startswith('enc.') else 1e-5
            state[normalization + '.running_mean'] = torch.zeros(outputs)
            state[normalization + '.running_var'] = torch.full((outputs,), 1 - epsilon)
            state[normalization + '.weight'] = values[weights + outputs:weights + 2 * outputs] if kind == POOL else torch.ones(outputs)
            state[normalization + '.bias'] = values[weights + 2 * outputs:] if kind == POOL else torch.zeros(outputs)
    return state


def write_fixture(path, golden):
    points = golden['points']
    positions = golden['positions']
    values = []
    for point in points:
        for offset in range(0, 12, 3):
            values += point[offset:offset + 3] + [0]
    for position in positions:
        values += position + [0]
    values += [value for row in golden['embeddings'] for value in row]
    Path(path).write_bytes(struct.pack('<II', len(points), len(positions)) + struct.pack('<' + 'f' * len(values), *values))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--encoder', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--decoder', type=Path)
    parser.add_argument('--refresh-golden', action='store_true')
    arguments = parser.parse_args()
    golden_path = Path(__file__).with_name('light_transformer_encoder_golden.json')
    write_encoder(arguments.encoder)
    if arguments.decoder:
        from light_transformer_fixture import make_data
        from ludus_tools.light_transformer import write_decoder, read_layout
        layout = read_layout(ROOT / 'modules/content/render_graph_format/include/ludus/content/render_graph_format/detail/light_transformer_layout.h')
        values = array.array('f', make_data(layout)[0])
        if sys.byteorder != 'little':
            values.byteswap()
        write_decoder(arguments.decoder, values, layout)
    if arguments.refresh_golden:
        import torch
        from light_transformer_encoder_oracle import make_points, run_encoder
        torch.set_num_threads(1)
        points = make_points(torch)
        with torch.inference_mode():
            positions, embeddings, _ = run_encoder(points, make_state(torch), torch)
        golden = {'points': points.tolist(), 'positions': positions.tolist(), 'embeddings': embeddings.tolist()}
        golden_path.write_text(json.dumps(golden, indent=2) + '\n')
    write_fixture(arguments.fixture, json.loads(golden_path.read_text()))


if __name__ == '__main__':
    main()
