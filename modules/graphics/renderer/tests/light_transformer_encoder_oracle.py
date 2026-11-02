"""Independent CPU PyTorch qualification of the released scene encoder.

This test utility is optional; it is never imported by the engine or installed SDK.
Thanks to Xu et al., A Generalizable Light Transport 3D Embedding for Global
Illumination (SIGGRAPH 2026), https://arxiv.org/abs/2510.18189, and Wu et al.,
Point Transformer V3 (CVPR 2024), https://arxiv.org/abs/2312.10035.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'scripts/python'))
from ludus_tools.light_transformer_encoder import encoder_schema, CHANNELS, DECODER_CHANNELS, DEPTHS


def make_points(torch, count=128):
    # Compact irregular grid exercises both noncentral sparse kernels and pooling.
    values = []
    width = 8 if count == 128 else 16
    height = 4 if count == 128 else 16
    for index in range(count):
        x = index % width
        y = (index // width) % height
        z = index // (width * height)
        values.append((0.3 + x * 0.009 + (index % 3) * 0.00017,
                       0.4 + y * 0.009 + (index % 5) * 0.00013,
                       0.5 + z * 0.009 + (index % 7) * 0.00011,
                       0, 1, 0, 0.2 + x * (0.07 if count == 128 else 0.03), 0.3, 0.6, 1 if index % 17 == 0 else 0, 0, 0))
    return torch.tensor(values, dtype=torch.float32)


def encode_hilbert(coordinate, depth):
    # Integer Skilling mapping; compare to the original vectorized implementation
    # in local qualification. No external implementation is bundled.
    words = list(coordinate)
    for bit in reversed(range(depth)):
        mask = 1 << bit
        lower = mask - 1
        for axis in range(3):
            if words[axis] & mask:
                words[0] ^= lower
            else:
                exchanged = (words[0] ^ words[axis]) & lower
                words[axis] ^= exchanged
                words[0] ^= exchanged
    gray = 0
    for bit in reversed(range(depth)):
        for axis in range(3):
            gray = gray * 2 + ((words[axis] >> bit) & 1)
    binary = gray
    while gray:
        gray >>= 1
        binary ^= gray
    return binary


def encode_morton(coordinate):
    return sum(((coordinate[axis] >> bit) & 1) << (bit * 3 + 2 - axis)
               for bit in range(16) for axis in range(3))


def run_encoder(points, state, torch):
    functional = torch.nn.functional
    layers = iter(encoder_schema())
    trace = []

    def tensor(name):
        return state[name].detach().cpu().float()

    def batch_norm(value, name, epsilon):
        return functional.batch_norm(value, tensor(name + '.running_mean'), tensor(name + '.running_var'),
                                     tensor(name + '.weight'), tensor(name + '.bias'), training=False, eps=epsilon)

    def affine(value, expected=None, activation=None):
        kind, name, inputs, outputs, kernel, normalization, convolution = next(layers)
        if expected is not None:
            assert name == expected, (name, expected)
        weight = tensor(name + '.weight').reshape(outputs, inputs)
        bias = tensor(name + '.bias') if name + '.bias' in state else None
        value = functional.linear(value, weight, bias)
        if normalization:
            value = batch_norm(value.reshape(-1, outputs), normalization, 1e-3 if name.startswith('enc.') else 1e-5).reshape(value.shape)
        return activation(value) if activation else value

    def normalize(value):
        kind, name, inputs, outputs, kernel, normalization, convolution = next(layers)
        assert kind == 2
        return functional.layer_norm(value, (inputs,), tensor(name + '.weight'), tensor(name + '.bias'), eps=1e-5)

    def sparse(value, grid, activation=None):
        kind, name, inputs, outputs, kernel, normalization, convolution = next(layers)
        assert kind == 3
        weight = tensor(name + '.weight')
        bias = tensor(name + '.bias') if name + '.bias' in state else torch.zeros(outputs)
        result = bias.repeat(len(value), 1)
        offsets = grid[None, :, :] - grid[:, None, :] + kernel // 2
        valid = ((offsets >= 0) & (offsets < kernel)).all(-1)
        rows, columns = valid.nonzero(as_tuple=True)
        selected = offsets[rows, columns]
        kernels = (selected[:, 0] * kernel + selected[:, 1]) * kernel + selected[:, 2]
        for index in kernels.unique():
            mask = kernels == index
            products = functional.linear(value[columns[mask]], weight.reshape(outputs, kernel ** 3, inputs)[:, index, :])
            result.index_add_(0, rows[mask], products)
        if normalization:
            result = batch_norm(result, normalization, 1e-3)
        return activation(result) if activation else result

    def patches(grid, order_index, depth):
        coordinates = grid.tolist()
        if order_index % 2:
            coordinates = [(y, x, z) for x, y, z in coordinates]
        codes = [encode_hilbert(value, depth) if order_index >= 2 else encode_morton(value)
                 for value in coordinates]
        order = sorted(range(len(codes)), key=lambda index: (codes[index], index))
        patch = 1024
        padded = list(order)
        if len(order) > patch and len(order) % patch:
            padded += order[len(order) - patch:((len(order) + patch - 1) // patch) * patch - patch]
        return torch.tensor(padded), torch.argsort(torch.tensor(order)), min(len(order), patch)

    def block(value, grid, order, depth):
        value = value + normalize(affine(sparse(value, grid)))
        residual = value
        normalized = normalize(value)
        channels = value.shape[-1]
        heads = channels // (32 if channels == 128 else 16)
        width = channels // heads
        qkv = affine(normalized)
        indices, inverse, patch_size = patches(grid, order, depth)
        qkv = qkv[indices].reshape(-1, patch_size, 3, heads, width).permute(2, 0, 3, 1, 4)
        query, key, values = qkv.unbind(0)
        scores = (query / width ** 0.5) @ key.transpose(-2, -1)
        attended = (scores.softmax(-1) @ values).transpose(1, 2).reshape(-1, channels)
        value = residual + affine(attended[inverse])
        value = value + affine(affine(normalize(value), activation=functional.gelu))
        trace.append(value.detach().clone())
        return value

    def fps(position):
        indices = []
        distances = torch.full((len(position),), 1e10)
        selected = 0
        eligible = (position * position).sum(-1) > 1e-3
        for _ in range(len(position) // 2):
            indices.append(selected)
            distances = torch.minimum(distances, ((position - position[selected]) ** 2).sum(-1))
            selected = int(distances.masked_fill(~eligible, -1).argmax())
        return torch.tensor(indices)

    position = points[:, :3]
    value = affine(affine(points, activation=functional.relu), activation=functional.relu)
    for stage in range(2):
        centers = fps(position)
        selected = position[centers]
        distances = ((selected[:, None] - position[None]) ** 2).sum(-1)
        neighbors = distances.argsort(dim=-1, stable=True)[:, :32]
        grouped = torch.cat((value[neighbors] - value[centers, None], value[centers, None]), dim=1)
        value = affine(affine(grouped, activation=functional.relu), activation=functional.relu).max(1).values
        position = selected
    anchors = position.clone()
    grid = torch.div(position - position.min(0).values, 0.01, rounding_mode='trunc').to(torch.int64)
    depth = int(grid.max()).bit_length()
    value = sparse(value, grid, functional.gelu)
    parents = []
    for stage in range(5):
        if stage:
            old_grid = grid
            grid = grid >> (1 if depth else 0)
            depth = max(depth - 1, 0)
            codes = torch.tensor([encode_morton(coordinate) for coordinate in grid.tolist()])
            unique, inverse = codes.unique(sorted=True, return_inverse=True)
            # Projection precedes max, normalization follows max.
            kind, name, inputs, outputs, kernel, normalization, convolution = next(layers)
            projected = functional.linear(value, tensor(name + '.weight'), tensor(name + '.bias'))
            pooled = torch.stack([projected[inverse == index].max(0).values for index in range(len(unique))])
            value = functional.gelu(batch_norm(pooled, normalization, 1e-3))
            parent = parents[-1]
            parent.append(inverse)
            grid = torch.stack([grid[(inverse == index).nonzero()[0, 0]] for index in range(len(unique))])
        for index in range(DEPTHS[stage]):
            value = block(value, grid, index % 4, depth)
        parents.append([value, grid, depth])
    for stage in reversed(range(4)):
        parent, grid, depth, inverse = parents[stage]
        value = affine(value, activation=functional.gelu)[inverse] + affine(parent, activation=functional.gelu)
        for index in range(2):
            value = block(value, grid, index, depth)
    assert next(layers, None) is None
    return anchors, value, trace


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--points', type=int, default=128)
    arguments = parser.parse_args()
    import torch
    import numpy
    from ludus_tools.light_transformer import load_checkpoint
    torch.set_num_threads(1)
    state, digest = load_checkpoint(arguments.checkpoint, torch, numpy)
    points = make_points(torch, arguments.points)
    with torch.inference_mode():
        positions, embeddings, trace = run_encoder(points, state, torch)
    def vector4(values):
        return torch.cat((values, torch.zeros(len(values), 1)), -1).numpy().astype('<f4').tobytes()
    records = torch.stack([torch.cat((points[:, offset:offset + 3], torch.zeros(len(points), 1)), -1)
                           for offset in range(0, 12, 3)], 1).numpy().astype('<f4').tobytes()
    arguments.output.write_bytes(struct.pack('<II', len(points), len(positions)) + records +
                                 vector4(positions) + embeddings.numpy().astype('<f4').tobytes())
    print(f'Qualified CPU oracle: {len(trace)} blocks, {len(positions)} anchors; SHA-256 {digest}')


if __name__ == '__main__':
    main()
