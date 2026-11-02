#!/usr/bin/env python3
"""Check raw connection contributions, analytic map derivatives and SMS solves."""
import argparse
import json
from pathlib import Path

import numpy as np

from connection import evaluate_connection, evaluate_reflection_map
from gpu import compile_entry, pack_scene
from scene import SOURCE, Scene, read_gold_ior
from sms import prepare_smoothing, solve_two_stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=False)
    scene = Scene(arguments.upstream / SOURCE['directory'])
    eta, extinction = read_gold_ior(arguments.upstream)
    packed_scene, slopes = pack_scene(scene, arguments.upstream)
    random = np.random.default_rng(517)
    inputs = np.zeros((128, 5, 4), dtype=np.float32)
    inputs[:, 0, :2] = random.uniform(.05, .95, (len(inputs), 2))
    inputs[:, 0, 2] = np.resize(SOURCE['roughness'], len(inputs))
    inputs[:, 1, :3] = random.uniform([-10, 0, 5], [10, 0, 20], (len(inputs), 3))
    inputs[:, 2, :2] = random.uniform(0, 1, (len(inputs), 2))
    inputs[:, 3, :3] = random.uniform(.05, 1, (len(inputs), 3))
    coordinates = inputs[:, 0, :2]
    receiver = inputs[:, 1, :3]
    light = (2*inputs[:, 2, :2]-1) @ scene.transforms[0, :3, :2].T+scene.transforms[0, :3, 3]
    residual, jacobian = evaluate_reflection_map(scene, receiver, light, coordinates)
    expected = np.zeros((len(inputs), 3, 4))
    expected[:, 0] = np.c_[residual, jacobian[:, :, 0]]
    expected[:, 1] = np.c_[jacobian[:, :, 1], np.ones(len(inputs)), np.zeros(len(inputs))]
    expected[:, 2, :3] = evaluate_connection(scene, receiver, inputs[:, 3, :3], coordinates,
                                             inputs[:, 2, :2], inputs[:, 0, 2], eta, extinction)
    # Independent central differences check the analytic CPU derivatives.
    epsilon = 1e-7
    finite = np.stack([(evaluate_reflection_map(scene, receiver, light, coordinates+np.eye(2)[axis]*epsilon)[0] -
                       evaluate_reflection_map(scene, receiver, light, coordinates-np.eye(2)[axis]*epsilon)[0])/(2*epsilon)
                      for axis in range(2)], axis=-1)
    derivative_check = bool(np.allclose(jacobian, finite, rtol=2e-5, atol=1e-6))
    from wgpu.utils import get_default_device
    from wgpu.utils.compute import compute_with_buffers
    device = get_default_device()
    adapter = dict(device.adapter.info)
    if adapter['adapter_type'] == 'CPU':
        raise ValueError('Physical GPU required.')
    shader = compile_entry(arguments.output / 'transport', arguments.compiler, 'reflection_map.slang', 'ValidateFigure17Transport')
    result = compute_with_buffers({0: inputs, 2: slopes, 3: packed_scene}, {1: expected.size*4},
                                  shader.replace('fn ValidateFigure17Transport(', 'fn main('), n=((len(inputs)+31)//32, 1, 1))
    observed = np.frombuffer(result[1], dtype=np.float32).reshape(expected.shape)
    np.savez(arguments.output / 'transport.npz', inputs=inputs, expected=expected, observed=observed, finite_difference=finite)
    checks = dict(analytic_jacobian=derivative_check,
                  reflection_map=bool(np.allclose(observed[:, 0], expected[:, 0], rtol=5e-4, atol=2e-4)),
                  reflection_map_second_column=bool(np.allclose(observed[:, 1], expected[:, 1], rtol=5e-4, atol=2e-4)),
                  contribution=bool(np.allclose(observed[:, 2], expected[:, 2], rtol=2e-3, atol=1e-7)))
    # Solver parity uses fixed external random choices, independent of GPU RNG.
    inputs[:, 0, :2] = random.normal(size=(len(inputs), 2))*inputs[:, 0, 2, None]/np.sqrt(2)
    inputs[:, 2, :3] = light
    inputs[:, 3, :2] = random.uniform(.01, .99, (len(inputs), 2))
    inputs[:, 3, 2:] = random.normal(size=(len(inputs), 2))
    expected_solver = np.zeros_like(expected)
    smoothing = prepare_smoothing(scene)
    for index, values in enumerate(inputs):
        root = solve_two_stage(scene, values[1, :3], values[2, :3], values[0, :2],
                               values[3, :2], values[3, 2:], smoothing)
        if root is not None:
            expected_solver[index, 0, :3] = [*root, 1]
    shader = compile_entry(arguments.output / 'solver', arguments.compiler, 'sms.slang', 'ValidateFigure17Solver')
    result = compute_with_buffers({0: inputs, 2: slopes, 3: packed_scene}, {1: expected_solver.size*4},
                                  shader.replace('fn ValidateFigure17Solver(', 'fn main('), n=((len(inputs)+31)//32, 1, 1))
    observed_solver = np.frombuffer(result[1], dtype=np.float32).reshape(expected.shape)
    np.savez(arguments.output / 'solver.npz', inputs=inputs, expected=expected_solver, observed=observed_solver)
    is_successful = observed_solver[:, 0, 2] > 0
    values, _ = evaluate_reflection_map(scene, inputs[is_successful, 1, :3], inputs[is_successful, 2, :3],
                                         observed_solver[is_successful, 0, :2])
    constraint_errors = np.linalg.norm(values-inputs[is_successful, 0, :2], axis=1)
    checks['gpu_roots_satisfy_cpu_constraints'] = bool(np.all(constraint_errors < 5e-6))
    checks['gpu_roots_inside_surface'] = bool(np.all((observed_solver[is_successful, 0, :2] >= 0) &
                                                    (observed_solver[is_successful, 0, :2] < 1)))
    different = np.max(np.abs(observed_solver[:, 0, :3]-expected_solver[:, 0, :3]), axis=1) > 1e-5
    report = dict(adapter=adapter, checks=checks, source=SOURCE, samples=len(inputs),
                  solver_successes=int(expected_solver[:, 0, 2].sum()),
                  gpu_solver_successes=int(is_successful.sum()), different_solver_outcomes=int(different.sum()),
                  maximum_cpu_constraint_residual=float(constraint_errors.max()),
                  scope='connection/Jacobian parity and independent CPU validation of GPU roots; not identical Newton basins',
                  solver_note='FP32/FP64 Newton basins can differ. Each re-hit estimator must use its own solver; these checks alone do not prove estimator mean parity.')
    (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    if not all(checks.values()):
        raise ValueError('Transport/solver parity failed; inspect preserved arrays.')


if __name__ == '__main__':
    main()
