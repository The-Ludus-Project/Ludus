#!/usr/bin/env python3
"""Train the released PureLight receiver-flow architecture on the shared panel.

This is a controlled analytic-fixture adaptation of PureLight section 3.4, not
an execution of its pretrained rough-flower appearance model or its Fast variant.
Upstream code is imported from a separately acquired, pinned checkout; no upstream
source or checkpoints are redistributed. CUDA/tiny-cuda-nn remain external.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

import numpy as np

from benchmark import CENTER, HALF_EXTENT, Panel, PROBES, UPSTREAM_REVISION


class ReceiverFlow:
    def __init__(self, upstream, checkpoint, panel, seed=871):
        if upstream is None:
            raise ValueError('Provide --upstream pointing to the pinned official PureLight checkout.')
        revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != UPSTREAM_REVISION:
            raise ValueError(f'PureLight revision must be {UPSTREAM_REVISION}; got {revision}.')
        sys.path.insert(0, str(upstream.resolve()))
        import torch
        if not torch.cuda.is_available():
            raise RuntimeError('PureLight requires a CUDA GPU. Run this adapter on a CUDA host; no substitute sampler was run.')
        torch.manual_seed(seed)
        from models.real_NVP_PW_Quadratic import RealNVP_PW_Quadratic_TCNN
        from utils import importance_sampling_sphcap
        self.torch = torch
        self.cap = importance_sampling_sphcap
        self.panel = panel
        self.model = RealNVP_PW_Quadratic_TCNN(samples_dim=2, condition_dim=4, L=4,
            n_layers=5, h_dim=128, activation='relu', num_bins=32,
            samples_enc='OneBlob', condition_enc='DenseGrid(3)+OneBlob(1)', channel_conditioning=False).cuda()
        self.model.eval()
        if checkpoint:
            payload = torch.load(checkpoint, map_location='cuda', weights_only=True)
            if payload['fixture_sha256'] != panel.identity() or payload['upstream_revision'] != revision:
                raise ValueError('Checkpoint fixture or PureLight source identity differs from this benchmark. Retrain it.')
            self.model.load_state_dict(payload['state_dict'])

    def tensor(self, values):
        return self.torch.as_tensor(values, dtype=self.torch.float32, device='cuda')

    def conditions(self, points):
        point = self.tensor(points)
        center = self.tensor(CENTER)
        radius = HALF_EXTENT * np.sqrt(2)
        angle = self.cap.theta_c_from_point(point, center, radius)
        condition = self.cap.build_condition(point, center, max_distance=15 * radius,
            include_theta_c=True, theta_c=angle)
        return center - point, angle, condition

    def sample(self, point, count):
        torch = self.torch
        with torch.no_grad():
            axis, angle, condition = self.conditions(np.repeat(point[None], count, axis=0))
            uv, log_density = self.model.sample_logprob(count, c=condition, device='cuda')
            directions = self.cap.cap_uv_to_dir(uv, axis, angle, parameterization='disk_concentric')
            jacobian = self.cap.cap_uv_jacobian(uv, angle, parameterization='disk_concentric')
            directions = directions.cpu().numpy().astype(np.float64)
            solid_angle_density = (log_density.exp() / jacobian).cpu().numpy().reshape(-1)
            torch.cuda.synchronize()
        distance = np.divide(CENTER[1] - point[1], directions[:, 1],
                             out=np.zeros(count), where=directions[:, 1] > 0)
        area = (point[None, [0, 2]] + distance[:, None] * directions[:, [0, 2]]) / HALF_EXTENT
        valid = (distance > 0) & np.all((area >= -1) & (area < 1), axis=-1)
        _, area_jacobian, _ = self.panel.evaluate(point, area)
        return area, solid_angle_density * area_jacobian, valid

    def train(self, steps, seed):
        torch = self.torch
        torch.manual_seed(seed)
        rng = np.random.default_rng(seed)
        optimizer = torch.optim.AdamW(self.model.parameters(), lr=.001)
        self.model.train()
        beginning = time.perf_counter()
        # Forward KL, normalized separately at each receiver. Sample the exact
        # radiance target using the supported slice/area mixture as a teacher.
        for step in range(steps):
            point = PROBES[step % len(PROBES)]
            area, density, valid = self.panel.sample(point, 1024, rng, 'slice')
            radiance, jacobian, directions = self.panel.evaluate(point, area)
            weights = radiance.mean(axis=1) * jacobian / density
            weights[~valid] = 0
            total = weights.sum()
            if not total > 0:
                continue
            axis, angle, condition = self.conditions(np.repeat(point[None], len(area), axis=0))
            uv, _, is_inside = self.cap.cap_dir_to_uv(self.tensor(directions), axis, angle,
                                                      parameterization='disk_concentric')
            selected = self.tensor(weights / total) * is_inside
            optimizer.zero_grad()
            loss = -(selected * self.model.log_prob(uv, c=condition).reshape(-1)).sum()
            if not torch.isfinite(loss):
                raise ValueError(f'Nonfinite training loss at step {step}; preserve this run and investigate.')
            loss.backward()
            torch.nn.utils.clip_grad_norm_(self.model.parameters(), 2)
            optimizer.step()
        torch.cuda.synchronize()
        self.model.eval()
        return time.perf_counter() - beginning


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--steps', type=int, default=10000)
    parser.add_argument('--seed', type=int, default=871)
    args = parser.parse_args()
    if args.steps < 1 or args.output.exists():
        parser.error('Use positive --steps and a new --output directory.')
    panel = Panel()
    flow = ReceiverFlow(args.upstream, None, panel, args.seed)
    seconds = flow.train(args.steps, args.seed)
    args.output.mkdir(parents=True)
    flow.torch.save(dict(fixture_sha256=panel.identity(), upstream_revision=UPSTREAM_REVISION,
                         state_dict=flow.model.state_dict()), args.output / 'receiver-flow.pt')
    (args.output / 'training.json').write_text(json.dumps(dict(steps=args.steps, seed=args.seed,
        seconds=seconds, fixture_sha256=panel.identity(), upstream_revision=UPSTREAM_REVISION,
        scope='analytic panel adaptation; trained on eight fixed receivers; no generalization claim'), indent=2) + '\n')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, ImportError) as error:
        print(f'PureLight setup failed: {error}', file=sys.stderr)
        raise SystemExit(1) from error
