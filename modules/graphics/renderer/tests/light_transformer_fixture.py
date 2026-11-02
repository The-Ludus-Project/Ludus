"""Independent PyTorch decoder oracle and dependency-free deterministic fixture writer.

No upstream weights/data are included. --checkpoint permits local release-weight
qualification; --refresh recomputes the small synthetic golden with PyTorch.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'scripts/python'))
from ludus_tools.light_transformer import read_layout, pack_decoder, load_checkpoint

LAYOUT_PATH = ROOT / 'modules/content/render_graph_format/include/ludus/content/render_graph_format/detail/light_transformer_layout.h'
GOLDEN_PATH = Path(__file__).with_name('light_transformer_golden.json')


def make_data(layout):
    state = 42

    def random_scalar():
        nonlocal state
        state = (state * 1664525 + 1013904223) & 0xffffffff
        return ((state >> 8) & 0xffff) / 65536 - 0.5

    dimension = layout['DIMENSION']
    attention = layout['ATTENTION_CHANNELS']
    hidden = layout['FEED_FORWARD_CHANNELS']
    affine = lambda inputs, outputs: (inputs + 1) * outputs
    pre_size = affine(layout['QUERY_CHANNELS'], dimension) + affine(dimension, dimension)
    block_size = (4 * dimension + 3 * dimension * dimension + affine(3, attention) +
                  2 * affine(attention, dimension) + affine(dimension, attention) +
                  affine(dimension, dimension) + affine(dimension, hidden) + affine(hidden, dimension))
    count = pre_size + layout['DEPTH'] * block_size + affine(dimension, 3)
    weights = [random_scalar() / 4 for _ in range(count)]
    for block in range(layout['DEPTH']):
        offset = pre_size + block * block_size
        for normalization in (0, 2 * dimension):
            for channel in range(dimension):
                weights[offset + normalization + channel] += 1
    anchors = [[random_scalar() * 8 for _ in range(3)] + [0] +
               [random_scalar() * 2 for _ in range(dimension)] for _ in range(37)]
    anchors[1][:3] = anchors[0][:3]
    queries = [[random_scalar() * 8 for _ in range(3)] + [0] +
               [random_scalar() * 2 for _ in range(3)] + [0] +
               [random_scalar() + 0.5 for _ in range(3)] + [0] for _ in range(5)]
    queries[0][:3] = anchors[0][:3]
    # The binary format is binary32, including weights whose scale was offset.
    def round_values(values):
        return list(struct.unpack('<' + 'f' * len(values), struct.pack('<' + 'f' * len(values), *values)))
    return round_values(weights), [round_values(row) for row in anchors], [round_values(row) for row in queries]


def evaluate(weights, anchors, queries, layout):
    import torch
    import torch.nn.functional as functional
    torch.set_num_threads(1)
    dimension = layout['DIMENSION']
    hidden = layout['ATTENTION_CHANNELS']
    weights = torch.tensor(weights, dtype=torch.float32)
    anchors = torch.tensor(anchors, dtype=torch.float32)
    queries = torch.tensor(queries, dtype=torch.float32)
    query_position = queries[:, :3]
    features = torch.cat((query_position, functional.normalize(queries[:, 4:7], dim=-1), queries[:, 8:11]), dim=-1)
    # Stable exact-distance ordering declares the portable adapter's tie rule.
    indices = ((query_position[:, None] - anchors[None, :, :3]) ** 2).sum(-1).argsort(stable=True)[:, :layout['NEIGHBORS']]
    latents = anchors[indices, 4:]
    positions = anchors[indices, :3] - query_position[:, None]
    cursor = 0

    def take(count):
        nonlocal cursor
        result = weights[cursor:cursor + count]
        cursor += count
        return result

    def affine(inputs, outputs, has_bias=True):
        matrix = take(inputs * outputs).reshape(outputs, inputs)
        bias = take(outputs) if has_bias else None
        return lambda values: functional.linear(values, matrix, bias)

    first = affine(layout['QUERY_CHANNELS'], dimension)
    second = affine(dimension, dimension)
    embedding = second(first(features).relu()).relu()
    for _ in range(layout['DEPTH']):
        attention_scale = take(dimension)
        attention_bias = take(dimension)
        feed_forward_scale = take(dimension)
        feed_forward_bias = take(dimension)
        project_query = affine(dimension, dimension, False)
        project_key = affine(dimension, dimension, False)
        project_value = affine(dimension, dimension, False)
        position_first = affine(3, hidden)
        position_second = affine(hidden, dimension)
        attention_first = affine(dimension, hidden)
        attention_second = affine(hidden, dimension)
        attention_output = affine(dimension, dimension)
        feed_forward_first = affine(dimension, layout['FEED_FORWARD_CHANNELS'])
        feed_forward_second = affine(layout['FEED_FORWARD_CHANNELS'], dimension)
        query = project_query(functional.layer_norm(embedding, (dimension,), attention_scale, attention_bias,
                                                   layout['QUERY_NORM_EPSILON']))
        keys = project_key(latents)
        values = project_value(keys)
        position = position_second(position_first(positions).relu())
        probabilities = attention_second(attention_first(keys - query[:, None] + position).relu()).softmax(dim=1)
        embedding = embedding + attention_output(((values + position) * probabilities).sum(dim=1))
        normalized = functional.layer_norm(embedding, (dimension,), feed_forward_scale, feed_forward_bias,
                                           layout['QUERY_NORM_EPSILON'])
        embedding = embedding + feed_forward_second(feed_forward_first(normalized).relu())
    result = functional.softplus(affine(dimension, 3)(embedding))
    assert cursor == len(weights)
    return [row + [0] for row in result.tolist()]


def write_fixture(path, weights, anchors, queries, expected):
    scalars = weights + [value for row in anchors + queries + expected for value in row]
    header = struct.pack('<III', len(weights), len(anchors), len(queries))
    Path(path).write_bytes(header + struct.pack('<' + 'f' * len(scalars), *scalars))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--refresh', action='store_true')
    parser.add_argument('--checkpoint', type=Path)
    arguments = parser.parse_args()
    layout = read_layout(LAYOUT_PATH)
    weights, anchors, queries = make_data(layout)
    if arguments.checkpoint:
        import torch
        import numpy
        state, _ = load_checkpoint(arguments.checkpoint, torch, numpy)
        weights = pack_decoder(state, layout, numpy).tolist()
        expected = evaluate(weights, anchors, queries, layout)
    elif arguments.refresh:
        expected = evaluate(weights, anchors, queries, layout)
        GOLDEN_PATH.write_text(json.dumps(expected, indent=2) + '\n')
    else:
        expected = json.loads(GOLDEN_PATH.read_text())
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    write_fixture(arguments.output, weights, anchors, queries, expected)


if __name__ == '__main__':
    main()
