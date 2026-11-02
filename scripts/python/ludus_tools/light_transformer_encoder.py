"""Portable encoder parameter schema, independent of any tensor runtime at execution.

Thanks to Bing Xu et al., A Generalizable Light Transport 3D Embedding for Global
Illumination, SIGGRAPH 2026, section 3.3, https://arxiv.org/abs/2510.18189,
and Xiaoyang Wu et al., Point Transformer V3, CVPR 2024. This independently
adapts the released inference architecture; upstream code/assets are not bundled.
"""

import struct
from pathlib import Path

from .light_transformer import read_layout


def find_layout(name):
    candidates = (Path(__file__).resolve().parents[3] /
                  'modules/content/render_graph_format/include/ludus/content/render_graph_format/detail' / name,
                  Path(__file__).resolve().parents[2] / 'shaders' / name)
    return next((path for path in candidates if path.is_file()), candidates[0])


ENCODER_LAYOUT = read_layout(find_layout('light_transformer_encoder_layout.h'), 'LIGHT_ENCODER')
DECODER_LAYOUT = read_layout(find_layout('light_transformer_layout.h'))


MAGIC = ENCODER_LAYOUT['MAGIC']
VERSION = ENCODER_LAYOUT['VERSION']
HEADER_WORDS = ENCODER_LAYOUT['HEADER_WORDS']
RECORD_WORDS = ENCODER_LAYOUT['LAYER_WORDS']
AFFINE = ENCODER_LAYOUT['AFFINE']
NORMALIZE = ENCODER_LAYOUT['NORMALIZE']
CONVOLUTION = ENCODER_LAYOUT['CONVOLUTION']
POOL = ENCODER_LAYOUT['POOL']
DIMENSION = DECODER_LAYOUT['DIMENSION']
CHANNELS = (DIMENSION, DIMENSION, DIMENSION, 2 * DIMENSION, 2 * DIMENSION)
DECODER_CHANNELS = (DIMENSION, DIMENSION, 2 * DIMENSION, 2 * DIMENSION)
DEPTHS = (2, 2, 2, ENCODER_LAYOUT['MIDDLE_STAGE_DEPTH'], 2)


def encoder_schema():
    """Yield the exact layer sequence; dimensions are verified independently by the runtime."""
    def affine(name, inputs, outputs, normalization=None, convolution=False):
        return (AFFINE, name, inputs, outputs, 1, normalization, convolution)

    def block(prefix, channels):
        yield (CONVOLUTION, prefix + '.cpe.0', channels, channels, 3, None, False)
        yield affine(prefix + '.cpe.1', channels, channels)
        yield (NORMALIZE, prefix + '.cpe.2', channels, channels, 1, None, False)
        yield (NORMALIZE, prefix + '.norm1.0', channels, channels, 1, None, False)
        yield affine(prefix + '.attn.qkv', channels, 3 * channels)
        yield affine(prefix + '.attn.proj', channels, channels)
        yield (NORMALIZE, prefix + '.norm2.0', channels, channels, 1, None, False)
        yield affine(prefix + '.mlp.0.fc1', channels, 4 * channels)
        yield affine(prefix + '.mlp.0.fc2', 4 * channels, channels)

    for name, inputs, outputs in (('pre_fc_scene', 12, 32), ('group1_fc_scene', 32, 64),
                                   ('group2_fc_scene', 64, 128)):
        yield affine(name + '.0', inputs, outputs, name + '.1', True)
        yield affine(name + '.3', outputs, outputs, name + '.4', True)
    yield (CONVOLUTION, 'enc.embedding.stem.conv', 128, 128, 5, 'enc.embedding.stem.norm', False)
    for stage, (channels, depth) in enumerate(zip(CHANNELS, DEPTHS)):
        if stage:
            yield (POOL, f'enc.enc.enc{stage}.down.proj', CHANNELS[stage - 1], channels, 1,
                   f'enc.enc.enc{stage}.down.norm.0', False)
        for index in range(depth):
            yield from block(f'enc.enc.enc{stage}.block{index}', channels)
    for stage in reversed(range(4)):
        inputs = 256 if stage == 3 else DECODER_CHANNELS[stage + 1]
        prefix = f'enc.dec.dec{stage}.up'
        yield affine(prefix + '.proj.0', inputs, DECODER_CHANNELS[stage], prefix + '.proj.1')
        yield affine(prefix + '.proj_skip.0', CHANNELS[stage], DECODER_CHANNELS[stage], prefix + '.proj_skip.1')
        for index in range(2):
            yield from block(f'enc.dec.dec{stage}.block{index}', DECODER_CHANNELS[stage])


def pack_encoder(state, numpy):
    """Validate the complete encoder and fold only mathematically valid inference BN pairs."""
    used = set()
    values = []
    records = []
    cursor = 0

    def tensor(name, shape):
        source = state.get(name)
        if source is None or not hasattr(source, 'is_floating_point') or not source.is_floating_point():
            raise ValueError(f'{name}: missing floating-point encoder tensor; supply the released complete model')
        if tuple(source.shape) != tuple(shape):
            raise ValueError(f'{name}: expected {shape}, got {tuple(source.shape)}; supply the released architecture')
        result = source.detach().cpu().float().numpy().copy()
        if not numpy.isfinite(result).all():
            raise ValueError(f'{name}: nonfinite encoder parameters; repair the checkpoint')
        used.add(name)
        return result

    for kind, name, inputs, outputs, kernel, normalization, convolution in encoder_schema():
        if kind == NORMALIZE:
            pieces = [tensor(name + '.weight', (outputs,)), tensor(name + '.bias', (outputs,))]
        else:
            shape = ((outputs, kernel, kernel, kernel, inputs) if kind == CONVOLUTION else
                     (outputs, inputs, 1) if convolution else (outputs, inputs))
            weights = tensor(name + '.weight', shape).reshape(outputs, -1)
            has_bias = name + '.bias' in state
            bias = tensor(name + '.bias', (outputs,)) if has_bias else numpy.zeros(outputs, dtype=numpy.float32)
            pieces = [weights, bias]
            if normalization:
                scale = tensor(normalization + '.weight', (outputs,))
                shift = tensor(normalization + '.bias', (outputs,))
                mean = tensor(normalization + '.running_mean', (outputs,))
                variance = tensor(normalization + '.running_var', (outputs,))
                if (variance < 0).any():
                    raise ValueError(f'{normalization}: negative running variance; repair the checkpoint')
                epsilon = 1e-3 if name.startswith('enc.') else 1e-5
                factor = scale / numpy.sqrt(variance + epsilon)
                offset = shift - mean * factor
                if kind == POOL:
                    # Max pooling precedes BN. Folding a negative scale through
                    # max would change the network, so retain this affine pair.
                    pieces.extend((factor, offset))
                else:
                    pieces = [weights * factor[:, None], bias * factor + offset]
        packed = numpy.concatenate([piece.reshape(-1) for piece in pieces]).astype('<f4')
        if not numpy.isfinite(packed).all():
            raise ValueError(f'{name}: folded parameters overflow binary32; repair the checkpoint')
        records.append((kind, inputs, outputs, kernel, cursor, len(packed), 0, 0))
        values.append(packed)
        cursor += len(packed)
    expected = {name for name in state if name.startswith(('pre_fc_scene.', 'group1_fc_scene.',
                'group2_fc_scene.', 'enc.')) and not name.endswith('.num_batches_tracked')}
    if expected != used:
        raise ValueError(f'incompatible encoder tensors: {sorted(expected - used)}; export the released architecture')
    header = (MAGIC, VERSION, len(records), cursor, 0, 0, 0, 0)
    if len(records) != ENCODER_LAYOUT['LAYER_COUNT'] or cursor != ENCODER_LAYOUT['WEIGHT_COUNT']:
        raise ValueError('encoder schema differs from the shared representation; repair the exporter installation')
    metadata = struct.pack('<' + 'I' * HEADER_WORDS, *header)
    metadata += b''.join(struct.pack('<' + 'I' * RECORD_WORDS, *record) for record in records)
    return metadata + b''.join(value.tobytes() for value in values)
