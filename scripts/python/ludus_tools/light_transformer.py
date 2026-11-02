"""Offline checkpoint-to-portable-decoder adaptation; no training runtime in Ludus.

Thanks to Bing Xu et al., A Generalizable Light Transport 3D Embedding for Global
Illumination, SIGGRAPH 2026, section 3.3, https://arxiv.org/abs/2510.18189.
This independently packs the published model's decoder contract, including V(K).
Upstream code, weights and example data are not redistributed by this tool.
"""

import argparse
import hashlib
import json
import math
import pickle
import re
import struct
import tempfile
from pathlib import Path


RELEASE_SHA256 = 'd957187949f44c36415116436fe613e1061d940706b84565ef5b5e65fc56af7e'
HASH_CHUNK_BYTES = 1024 * 1024


def read_layout(path, prefix='LIGHT_TRANSFORMER'):
    """Read scalar representation constants from the shared host/shader owner."""
    return {name: int(value, 0) if '.' not in value else float(value)
            for name, value in re.findall(
                rf'^#define LUDUS_{prefix}_(\w+)\s+(0x[0-9a-fA-F]+|[0-9.]+)\s*$',
                Path(path).read_text(), re.MULTILINE)}


def load_checkpoint(path, torch, numpy):
    """Use PyTorch's restricted weights loader, never the unrestricted pickle path."""
    version = re.match(r'^(\d+)\.(\d+)', torch.__version__)
    if version is None or tuple(map(int, version.groups())) < (2, 6):
        raise ValueError('PyTorch 2.6+ is required for restricted checkpoint loading; upgrade the offline-tool environment')
    with Path(path).open('rb') as stream:
        if stream.read(128).startswith(b'version https://git-lfs.github.com/spec/'):
            raise ValueError('checkpoint is a Git LFS pointer; run git lfs pull in the authors repository to download the actual weights')
        stream.seek(0)
        hasher = hashlib.sha256()
        for chunk in iter(lambda: stream.read(HASH_CHUNK_BYTES), b''):
            hasher.update(chunk)
        digest = hasher.hexdigest()
    if digest == RELEASE_SHA256:
        # The released training checkpoint contains a NumPy scalar in optimizer
        # metadata. Permit only its exact reconstruction types for this digest.
        scalar = numpy._core.multiarray.scalar
        allowed = [(scalar, 'numpy.core.multiarray.scalar'), numpy.dtype,
                   type(numpy.dtype('float64')), type(numpy.dtype('int64')),
                   type(numpy.dtype('uint32'))]
        with torch.serialization.safe_globals(allowed):
            checkpoint = torch.load(path, map_location='cpu', weights_only=True)
    else:
        checkpoint = torch.load(path, map_location='cpu', weights_only=True)
    if not isinstance(checkpoint, dict):
        raise ValueError('checkpoint is not a dictionary; export a tensor-only state_dict')
    state = checkpoint.get('model', checkpoint)
    if not isinstance(state, dict) or not state:
        raise ValueError('checkpoint has no model tensor dictionary; export a state_dict or the model entry')
    if not all(isinstance(name, str) for name in state):
        raise ValueError('model tensor names must be strings; export a tensor-only state_dict')
    has_prefix = [name.startswith('module.') for name in state]
    if any(has_prefix) and not all(has_prefix):
        raise ValueError('mixed module prefixes; export one complete model state_dict')
    return {name.removeprefix('module.'): value for name, value in state.items()}, digest


def pack_decoder(state, layout, numpy):
    """Validate every consumed tensor and fold inference BatchNorm into query MLPs."""
    dimension = layout['DIMENSION']
    attention = layout['ATTENTION_CHANNELS']
    feed_forward = layout['FEED_FORWARD_CHANNELS']
    used = set()
    pieces = []

    def tensor(name, shape):
        if name not in state:
            raise ValueError(f'missing tensor {name}; supply the complete released 128-dimensional model')
        source = state[name]
        if not hasattr(source, 'is_floating_point') or not hasattr(source, 'shape'):
            raise ValueError(f'{name}: expected a floating-point tensor; export a tensor-only state_dict')
        if tuple(source.shape) != tuple(shape) or not source.is_floating_point():
            raise ValueError(f'{name}: expected floating-point shape {shape}, got {tuple(source.shape)}')
        value = source.detach().to(device='cpu').float().numpy().copy()
        if not numpy.isfinite(value).all():
            raise ValueError(f'{name}: nonfinite parameters; repair or re-export the checkpoint')
        used.add(name)
        return value

    def append(value):
        value = numpy.asarray(value, dtype='<f4').reshape(-1)
        if not numpy.isfinite(value).all():
            raise ValueError('folded parameters overflow binary32; repair the checkpoint')
        pieces.append(value)

    def affine(name, inputs, outputs, has_bias=True):
        append(tensor(f'{name}.weight', (outputs, inputs)))
        if has_bias:
            append(tensor(f'{name}.bias', (outputs,)))

    def norm(name):
        append(tensor(f'{name}.weight', (dimension,)))
        append(tensor(f'{name}.bias', (dimension,)))

    for convolution, normalization, inputs in ((0, 1, layout['QUERY_CHANNELS']), (3, 4, dimension)):
        weights = tensor(f'pre_fc_query.{convolution}.weight', (dimension, inputs, 1))[:, :, 0]
        prefix = f'pre_fc_query.{normalization}'
        scale = tensor(f'{prefix}.weight', (dimension,))
        bias = tensor(f'{prefix}.bias', (dimension,))
        mean = tensor(f'{prefix}.running_mean', (dimension,))
        variance = tensor(f'{prefix}.running_var', (dimension,))
        if (variance < 0).any():
            raise ValueError(f'{prefix}: negative running variance; repair the checkpoint')
        factor = scale / numpy.sqrt(variance + layout['BATCH_NORM_EPSILON'])
        append(weights * factor[:, None])
        append(bias - mean * factor)

    for block in range(layout['DEPTH']):
        prefix = f'dec_blocks.{block}'
        norm(f'{prefix}.attn_norm')
        norm(f'{prefix}.ff_norm')
        for projection in ('q_fc', 'k_fc', 'v_fc'):
            affine(f'{prefix}.attn.{projection}', dimension, dimension, False)
        affine(f'{prefix}.attn.pos_fc.0', layout['POSITION_CHANNELS'], attention)
        affine(f'{prefix}.attn.pos_fc.2', attention, dimension)
        affine(f'{prefix}.attn.attn_fc.0', dimension, attention)
        affine(f'{prefix}.attn.attn_fc.2', attention, dimension)
        affine(f'{prefix}.attn.out_fc', dimension, dimension)
        affine(f'{prefix}.ff.fc1', dimension, feed_forward)
        affine(f'{prefix}.ff.fc2', feed_forward, dimension)
    affine('out_fc.0', dimension, layout['OUTPUT_CHANNELS'])
    # Refuse unrecognized decoder parameters rather than silently ignoring a
    # different architecture. Encoder tensors and unused BN counters stay external.
    decoder_names = {name for name in state if name.startswith(('pre_fc_query.', 'dec_blocks.', 'out_fc.'))
                     and not name.endswith('.num_batches_tracked')}
    if decoder_names != used:
        raise ValueError(f'incompatible decoder tensors: {sorted(decoder_names - used)}; export the released architecture')
    return numpy.concatenate(pieces).astype('<f4', copy=False)


def write_decoder(path, values, layout):
    """Publish a complete asset atomically; preserve an existing file on export failure."""
    header = [layout['FILE_MAGIC'], layout['FILE_VERSION'], len(values)]
    header.extend([0] * (layout['FILE_HEADER_WORDS'] - len(header)))
    data = struct.pack('<' + 'I' * len(header), *header) + values.tobytes()
    write_asset(path, data)


def write_asset(path, data):
    """Atomically publish one validated parameter asset."""
    destination = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=destination.name + '.', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        temporary.replace(destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def is_representable_training_scale(value):
    """Require finite positive binary32 scale and reciprocal for GPU normalization."""
    if value is None or not math.isfinite(value) or value <= 0:
        return False
    try:
        scale = struct.unpack('<f', struct.pack('<f', value))[0]
        inverse = struct.unpack('<f', struct.pack('<f', 1 / scale))[0]
    except (OverflowError, ZeroDivisionError):
        return False
    return math.isfinite(scale) and scale > 0 and math.isfinite(inverse) and inverse > 0

def main(argv=None):
    parser = argparse.ArgumentParser(description='Adapt trained Light Transformer encoder/decoder weights to portable Ludus compute.')
    parser.add_argument('checkpoint', type=Path)
    parser.add_argument('--decoder-output', type=Path)
    parser.add_argument('--encoder-output', type=Path)
    parser.add_argument('--model-directory', type=Path, help='export encoder.llte, decoder.lltd and model.json for the render graph template')
    parser.add_argument('--world-units-per-model-unit', type=float, help='fixed training dataset scale, required with --model-directory')
    # Either or both adapters may be requested; validation precedes publication.
    candidates = (Path(__file__).resolve().parents[3] /
                  'modules/content/render_graph_format/include/ludus/content/render_graph_format/detail/light_transformer_layout.h',
                  Path(__file__).resolve().parents[2] / 'shaders/light_transformer_layout.h')
    parser.add_argument('--layout', type=Path, default=next((path for path in candidates if path.is_file()), candidates[0]))
    arguments = parser.parse_args(argv)
    if arguments.model_directory is not None:
        if arguments.decoder_output is not None or arguments.encoder_output is not None:
            parser.error('--model-directory owns both outputs; omit individual output paths')
        if is_representable_training_scale(arguments.world_units_per_model_unit) is False:
            parser.error('--model-directory requires a positive binary32 scale and reciprocal matching the training dataset')
        arguments.encoder_output = arguments.model_directory / 'encoder.llte'
        arguments.decoder_output = arguments.model_directory / 'decoder.lltd'
    if arguments.decoder_output is None and arguments.encoder_output is None:
        parser.error('select --model-directory, --encoder-output and/or --decoder-output')
    try:
        import torch
        import numpy
        layout = read_layout(arguments.layout)
        state, digest = load_checkpoint(arguments.checkpoint, torch, numpy)
        values = pack_decoder(state, layout, numpy) if arguments.decoder_output else None
        if arguments.encoder_output:
            from .light_transformer_encoder import pack_encoder
            encoder = pack_encoder(state, numpy)
        if arguments.model_directory:
            arguments.model_directory.mkdir(parents=True, exist_ok=True)
        if arguments.decoder_output:
            write_decoder(arguments.decoder_output, values, layout)
        if arguments.encoder_output:
            write_asset(arguments.encoder_output, encoder)
        if arguments.model_directory:
            from .light_transformer_encoder import ENCODER_LAYOUT
            def asset_digest(path):
                hasher = hashlib.sha256()
                with path.open('rb') as stream:
                    for chunk in iter(lambda: stream.read(HASH_CHUNK_BYTES), b''):
                        hasher.update(chunk)
                return hasher.hexdigest()
            configuration = {'version': 1,
                             'encoder_sha256': asset_digest(arguments.encoder_output),
                             'decoder_sha256': asset_digest(arguments.decoder_output), 'world_units_per_model_unit': arguments.world_units_per_model_unit,
                             'scene_points': ENCODER_LAYOUT['MAX_POINTS'], 'seed': 1,
                             'maximum_sampling_attempts': ENCODER_LAYOUT['MAX_POINTS'] * ENCODER_LAYOUT['DEFAULT_ATTEMPTS_PER_POINT'],
                             'maximum_buffer_bytes': 512 * 1024 * 1024,
                             'maximum_surface_preparation_bytes': 512 * 1024 * 1024,
                             'checkpoint_sha256': digest}
            write_asset(arguments.model_directory / 'model.json', (json.dumps(configuration, indent=2) + '\n').encode())
    except ImportError as error:
        parser.exit(1, f'Light Transformer export failed: {error}. Install PyTorch 2.6+ and NumPy 2+ in the offline-tool environment.\n')
    except pickle.UnpicklingError as error:
        parser.exit(1, f'Light Transformer export failed: {error}. Supply a tensor-only state_dict; unrestricted pickle loading is not supported.\n')
    except OSError as error:
        parser.exit(1, f'Light Transformer export failed: {error}. Check the input path and create a writable output parent directory.\n')
    except (ValueError, RuntimeError, TypeError) as error:
        parser.exit(1, f'Light Transformer export failed: {error}. Export a complete, finite state_dict for the released 128-dimensional decoder.\n')
    if arguments.decoder_output:
        print(f'Wrote {len(values)} finite decoder parameters to {arguments.decoder_output}; source SHA-256 {digest}.')
    if arguments.encoder_output:
        print(f'Wrote portable encoder parameters to {arguments.encoder_output}; source SHA-256 {digest}.')
    return 0
