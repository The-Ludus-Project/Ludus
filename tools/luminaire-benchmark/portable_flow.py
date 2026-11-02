"""Portable PureLight receiver-flow inference and CPU training.

Independent NumPy/Slang implementation of Figueiredo et al., PureLight (2026),
section 3.4, https://arxiv.org/abs/2606.04319. The CPU trainer uses the released
spline transform for differentiation. MLP/encoding evaluation is portable FP32.
Compared with TCNN: biases are explicit, dense-grid nodes start at zero and are
stored sparsely, and neither fused CUDA arithmetic nor checkpoint packing is used.
Unvisited grid nodes are exactly zero; sparsity changes storage, not interpolation.
See NOTICE.txt for source attribution and the upstream adaptation license.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import time

import numpy as np

from benchmark import CENTER, HALF_EXTENT, Panel, PROBES, UPSTREAM_REVISION

BINS = 32
LAYERS = 4
WIDTH = 128
HIDDEN = 5
LEVELS = 8
FEATURES = 8
GRID_KEY_STRIDE = 1 << 24
SCALES = (8 * np.power(1.405, np.arange(LEVELS)) - 1).astype(np.float32)
RESOLUTIONS = (np.ceil(SCALES) + 1).astype(np.int64)
RADIUS = HALF_EXTENT * math.sqrt(2)
MAX_DISTANCE = 15 * RADIUS
PARAMETER_COUNT = 2 * BINS + 1
INPUT_COUNT = BINS * 2 + LEVELS * FEATURES
SHAPES = [(WIDTH, INPUT_COUNT)] + [(WIDTH, WIDTH)] * (HIDDEN - 1) + [(PARAMETER_COUNT, WIDTH)]


def one_blob(value):
    value = np.asarray(value, dtype=np.float32).reshape(-1, 1)
    edges = np.arange(BINS + 1, dtype=np.float32)[None] / BINS
    def cdf(offset):
        u = np.clip(offset * BINS, -1, 1)
        return .5 + (15 / 16) * (u - (2 / 3) * u**3 + u**5 / 5)
    offsets = edges - value
    cumulative = cdf(offsets) + cdf(offsets - 1) + cdf(offsets + 1)
    return np.diff(cumulative, axis=1)


def condition(point):
    offset = np.asarray(point, dtype=np.float32) - CENTER.astype(np.float32)
    distance = np.linalg.norm(offset)
    if distance <= RADIUS or distance > MAX_DISTANCE:
        raise ValueError('Receiver must be outside the fixture sphere and inside its declared training domain.')
    angle = np.arcsin(np.float32(RADIUS / distance))
    return np.r_[offset / MAX_DISTANCE, angle / (math.pi / 2)].astype(np.float32)


def grid_corners(value):
    value = (np.asarray(value[:3], dtype=np.float32) + 1) * .5
    keys, weights = [], []
    for level, (scale, resolution) in enumerate(zip(SCALES, RESOLUTIONS)):
        coordinate = value * scale + .5
        lower = np.floor(coordinate).astype(np.int64)
        fraction = coordinate - lower
        fraction = fraction**2 * (3 - 2 * fraction)
        for corner in range(8):
            bits = np.array([(corner >> axis) & 1 for axis in range(3)])
            vertex = lower + bits
            flat = int(vertex[0] + resolution * (vertex[1] + resolution * vertex[2])) % int(resolution**3)
            keys.append(level * GRID_KEY_STRIDE + flat)
            weights.append(np.prod(np.where(bits, fraction, 1 - fraction)))
    return np.array(keys, dtype=np.int64), np.array(weights, dtype=np.float32)


def spline(value, parameters, inverse=False):
    """Normalized positive piecewise-linear PDF and its CDF/inverse CDF."""
    heights = np.exp(parameters[:, :BINS + 1])
    widths = np.exp(parameters[:, BINS + 1:])
    widths /= widths.sum(axis=1, keepdims=True)
    mass = .5 * (heights[:, :-1] + heights[:, 1:]) * widths
    heights /= mass.sum(axis=1, keepdims=True)
    mass = .5 * (heights[:, :-1] + heights[:, 1:]) * widths
    locations = np.c_[np.zeros(len(value)), np.cumsum(widths, axis=1)]
    cumulative = np.c_[np.zeros(len(value)), np.cumsum(mass, axis=1)]
    bounds = cumulative if inverse else locations
    indices = np.minimum((bounds[:, 1:] <= value[:, None]).sum(axis=1), BINS - 1)
    rows = np.arange(len(value))
    left = heights[rows, indices]
    delta = heights[rows, indices + 1] - left
    width = widths[rows, indices]
    if inverse:
        mass_offset = (value - cumulative[rows, indices]) / width
        # Rationalized quadratic root remains stable for equal endpoint heights.
        fraction = 2 * mass_offset / (left + np.sqrt(np.maximum(left**2 + 2 * delta * mass_offset, 0)))
        fraction = np.clip(fraction, 0, 1)
        result = locations[rows, indices] + width * fraction
    else:
        fraction = (value - locations[rows, indices]) / width
        result = cumulative[rows, indices] + width * (left * fraction + .5 * delta * fraction**2)
    log_pdf = np.log(left + delta * fraction)
    return result.astype(np.float32), log_pdf.astype(np.float32)


class PortableFlow:
    def __init__(self, path, panel):
        self.panel = panel
        with np.load(path, allow_pickle=False) as archive:
            self.arrays = {key: archive[key] for key in archive.files}
        self.metadata = json.loads(str(self.arrays.pop('metadata')))
        if self.metadata['fixture_sha256'] != panel.identity():
            raise ValueError('Portable flow belongs to another luminaire profile. Retrain before comparing.')
        self.keys = self.arrays['grid_keys']
        self.grid = self.arrays['grid_features']

    def encode(self, point):
        value = condition(point)
        keys, weights = grid_corners(value)
        indices = np.searchsorted(self.keys, keys)
        valid = indices < len(self.keys)
        indices = np.minimum(indices, len(self.keys) - 1)
        valid &= self.keys[indices] == keys
        features = self.grid[indices] * (weights * valid)[:, None]
        grid = features.reshape(LEVELS, 8, FEATURES).sum(axis=1).ravel()
        return np.r_[grid, one_blob((value[3] + 1) * .5)[0]].astype(np.float32)

    def network(self, layer, value, encoded):
        hidden = np.c_[one_blob(value), np.repeat(encoded[None], len(value), axis=0)]
        for index in range(len(SHAPES)):
            hidden = hidden @ self.arrays[f'w{layer}_{index}'].T + self.arrays[f'b{layer}_{index}']
            hidden = np.tanh(hidden) if index == HIDDEN else np.maximum(hidden, 0)
        return hidden

    def transform(self, point, samples, inverse=False):
        samples = np.array(samples, dtype=np.float32, copy=True)
        encoded = self.encode(point)
        log_pdf = np.zeros(len(samples), dtype=np.float32)
        for layer in (range(LAYERS - 1, -1, -1) if inverse else range(LAYERS)):
            off = layer % 2
            parameters = self.network(layer, samples[:, off], encoded)
            samples[:, 1-off], contribution = spline(samples[:, 1-off], parameters, inverse)
            log_pdf += contribution
        return samples, log_pdf

    def sample(self, point, count, seed=871):
        rng = np.random.default_rng(seed)
        results = []
        densities = []
        valid = []
        for offset in range(0, count, 1024):
            uv, log_pdf = self.transform(point, rng.random((min(count-offset, 1024), 2)), True)
            directions, cap_area = cap_directions(point, uv)
            distance = np.divide(CENTER[1] - point[1], directions[:, 1], out=np.zeros(len(uv)), where=directions[:, 1] > 0)
            area = (point[None, [0, 2]] + distance[:, None] * directions[:, [0, 2]]) / HALF_EXTENT
            _, jacobian, _ = self.panel.evaluate(point, area)
            results.append(area)
            densities.append(np.exp(log_pdf) * jacobian / cap_area)
            valid.append((distance > 0) & np.all((area >= -1) & (area < 1), axis=1))
        return np.concatenate(results), np.concatenate(densities), np.concatenate(valid)


def cap_directions(point, uv):
    axis = CENTER - point
    axis /= np.linalg.norm(axis)
    helper = np.array([0., 0, 1]) if abs(axis[2]) < .999 else np.array([1., 0, 0])
    tangent = np.cross(helper, axis)
    tangent /= np.linalg.norm(tangent)
    bitangent = np.cross(axis, tangent)
    square = 2 * uv - 1
    dominant = np.abs(square[:, 0]) > np.abs(square[:, 1])
    radius = np.where(dominant, square[:, 0], square[:, 1])
    safe_x = np.where(square[:, 0] == 0, 1, square[:, 0])
    safe_y = np.where(square[:, 1] == 0, 1, square[:, 1])
    angle = np.where(dominant, math.pi / 4 * square[:, 1] / safe_x,
                     math.pi / 2 - math.pi / 4 * square[:, 0] / safe_y)
    disk = radius[:, None] * np.c_[np.cos(angle), np.sin(angle)]
    cap_cosine = math.sqrt(1 - (RADIUS / np.linalg.norm(CENTER-point))**2)
    k = 1 - cap_cosine
    cosine = 1 - radius**2 * k
    sine_over_radius = np.sqrt(k * (2 - radius**2 * k))
    return (sine_over_radius[:, None] * (disk[:, :1] * tangent + disk[:, 1:] * bitangent)
            + cosine[:, None] * axis), 2 * math.pi * k


def train(upstream, destination, steps, seed):
    import torch
    from torch import nn
    torch.set_num_threads(4)
    torch.manual_seed(seed)
    revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != UPSTREAM_REVISION:
        raise ValueError('Use the pinned PureLight checkout for its differentiable spline reference.')
    sys.path.insert(0, str(upstream.resolve()))
    from models.real_NVP_PW_Quadratic import RealNVP_PW_Quadratic
    panel = Panel()
    conditions = [condition(point) for point in PROBES]
    keys = np.unique(np.concatenate([grid_corners(value)[0] for value in conditions]))
    features = nn.Parameter(torch.zeros(len(keys), FEATURES))
    networks = nn.ModuleList()
    for _ in range(LAYERS):
        components = []
        for index, (outputs, inputs) in enumerate(SHAPES):
            linear = nn.Linear(inputs, outputs)
            nn.init.xavier_uniform_(linear.weight, gain=.5)
            nn.init.zeros_(linear.bias)
            components += [linear, nn.Tanh() if index == HIDDEN else nn.ReLU()]
        networks.append(nn.Sequential(*components))
    optimizer = torch.optim.AdamW(list(networks.parameters()) + [features], lr=.001)
    rng = np.random.default_rng(seed)
    records = []
    # Training examples and cap conversion are prepared outside the measured training loop.
    for point, value in zip(PROBES, conditions):
        area, density, valid = panel.sample(point, 8192, rng, 'slice')
        radiance, jacobian, directions = panel.evaluate(point, area)
        weight = radiance.mean(axis=1) * jacobian / density
        weight[~valid] = 0
        # Use the same deterministic cap frame as the portable inference.
        uv = torch.tensor(cap_coordinates(point, directions), dtype=torch.float32)
        grid_keys, grid_weights = grid_corners(value)
        records.append((uv, torch.tensor(weight, dtype=torch.float32),
                        torch.tensor(np.searchsorted(keys, grid_keys)), torch.tensor(grid_weights),
                        torch.tensor(one_blob((value[3]+1)*.5)[0])))
    beginning = time.perf_counter()
    loss_value = None
    for step in range(steps):
        uv, weights, indices, interpolation, theta_blob = records[step % len(records)]
        batch = torch.tensor(rng.integers(0, len(uv), 512))
        selected = weights[batch]
        if selected.sum() <= 0:
            continue
        selected /= selected.sum()
        grid = (features[indices] * interpolation[:, None]).reshape(LEVELS, 8, FEATURES).sum(dim=1).ravel()
        encoded = torch.cat((grid, theta_blob))[None].expand(len(batch), -1)
        samples = uv[batch]
        log_pdf = torch.zeros(len(batch))
        for layer, network in enumerate(networks):
            off = layer % 2
            # The OneBlob is piecewise polynomial: preserve its input derivative.
            x = samples[:, off:off+1]
            edges = torch.arange(BINS+1)[None] / BINS
            def cdf(t):
                u = torch.clamp(t * BINS, -1, 1)
                return .5 + (15/16)*(u-(2/3)*u**3+u**5/5)
            offsets = edges-x
            cumulative = cdf(offsets)+cdf(offsets-1)+cdf(offsets+1)
            blob = cumulative[:, 1:]-cumulative[:, :-1]
            parameters = network(torch.cat((blob, encoded), dim=1))
            transformed, term = RealNVP_PW_Quadratic.piecewise_quadratic_transform(
                samples[:, 1-off:2-off], parameters[:, None])
            samples = torch.cat((samples[:, :1], transformed), dim=1) if off == 0 else torch.cat((transformed, samples[:, 1:]), dim=1)
            log_pdf = log_pdf + term
        loss = -(selected * log_pdf).sum()
        if not torch.isfinite(loss):
            raise ValueError(f'Nonfinite CPU training loss at step {step}')
        optimizer.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(list(networks.parameters()) + [features], 2)
        optimizer.step()
        loss_value = float(loss.detach())
    duration = time.perf_counter()-beginning
    arrays = dict(grid_keys=keys.astype(np.uint32), grid_features=features.detach().numpy())
    for layer, network in enumerate(networks):
        for index, linear in enumerate(network[::2]):
            arrays[f'w{layer}_{index}'] = linear.weight.detach().numpy()
            arrays[f'b{layer}_{index}'] = linear.bias.detach().numpy()
    metadata = dict(schema=1, fixture_sha256=panel.identity(), upstream_revision=revision,
                    steps=steps, seed=seed, training_seconds=duration, loss=loss_value,
                    arithmetic='portable-fp32', scope='analytic panel; eight training receivers; zero-initialized sparse dense grid')
    arrays['metadata'] = np.array(json.dumps(metadata))
    destination.mkdir(parents=True, exist_ok=False)
    np.savez(destination / 'flow.npz', **arrays)
    (destination / 'training.json').write_text(json.dumps(metadata, indent=2)+'\n')
    return destination / 'flow.npz'


def cap_coordinates(point, directions):
    axis = CENTER-point
    axis /= np.linalg.norm(axis)
    helper = np.array([0., 0, 1]) if abs(axis[2]) < .999 else np.array([1., 0, 0])
    tangent = np.cross(helper, axis)
    tangent /= np.linalg.norm(tangent)
    bitangent = np.cross(axis, tangent)
    cosine = directions @ axis
    k = 1-math.sqrt(1-(RADIUS/np.linalg.norm(CENTER-point))**2)
    radius = np.sqrt(np.maximum(1-cosine, 0)/k)
    phi = np.arctan2(directions @ bitangent, directions @ tangent)
    disk = radius[:, None]*np.c_[np.cos(phi), np.sin(phi)]
    x, y = disk.T
    dominant = np.abs(x) > np.abs(y)
    sign_x = np.where(x >= 0, 1, -1)
    sign_y = np.where(y >= 0, 1, -1)
    signed_x = sign_x*radius
    signed_y = sign_y*radius
    a = np.where(dominant, signed_x, signed_y*4/math.pi*np.arctan2(sign_y*x, np.abs(y)))
    b = np.where(dominant, signed_x*4/math.pi*np.arctan2(sign_x*y, np.abs(x)), signed_y)
    return np.clip((np.c_[a, b]+1)*.5, 1e-7, 1-1e-7).astype(np.float32)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--steps', type=int, default=1000)
    parser.add_argument('--seed', type=int, default=871)
    args = parser.parse_args()
    if args.steps < 1 or args.output.exists():
        parser.error('Use positive --steps and a fresh output directory.')
    print(train(args.upstream, args.output, args.steps, args.seed))


if __name__ == '__main__':
    main()
