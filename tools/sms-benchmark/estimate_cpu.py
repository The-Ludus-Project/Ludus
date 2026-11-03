#!/usr/bin/env python3
"""Independent CPU estimates/accumulation for selected Figure 17 floor pixels."""
import argparse
import json
from pathlib import Path

import numpy as np

from affine_slice import AffineSlice, DEFENSIVE_FRACTION
from connection import evaluate_connection, evaluate_reflection_map, make_point, make_receivers
from scene import SOURCE, Scene, normalize, read_gold_ior
from sms import MAXIMUM_BERNOULLI_TRIALS, REHIT_DISTANCE_UV, prepare_smoothing, solve_two_stage_many


def estimate(scene, receiver, color, method, alpha, count, random, eta, extinction, resolution, minimum_guide_alpha):
    light_coordinates = random.uniform(0, 1, (count, 2))
    samples = np.zeros((count, 3))
    caps = 0
    if method == 'sms':
        light = make_point(scene.transforms[0], light_coordinates)
        target = random.normal(size=(count, 2))*alpha/np.sqrt(2)
        smoothing = prepare_smoothing(scene)
        coordinates, success = solve_two_stage_many(scene, receiver, light, target, random.uniform(0, 1, (count, 2)),
                                                    random.normal(size=(count, 2)), smoothing)
        identities = np.flatnonzero(success)
        if len(identities) == 0:
            return samples, caps
        _, derivative = evaluate_reflection_map(scene, receiver, light[identities], coordinates[identities])
        determinant = np.abs(np.linalg.det(derivative))
        density = np.exp(-np.sum(target[identities]**2, axis=1)/alpha**2)/(np.pi*alpha**2)
        factor = evaluate_connection(scene, receiver, color, coordinates[identities], light_coordinates[identities], alpha, eta, extinction)/(density*determinant)[:, None]
        for trial in range(1, MAXIMUM_BERNOULLI_TRIALS+1):
            if len(identities) == 0:
                break
            proposed, found = solve_two_stage_many(scene, receiver, light[identities], target[identities],
                random.uniform(0, 1, (len(identities), 2)), random.normal(size=(len(identities), 2)), smoothing)
            match = found & (np.linalg.norm(proposed-coordinates[identities], axis=1) < REHIT_DISTANCE_UV)
            samples[identities[match]] = factor[match]*trial
            identities = identities[~match]
            factor = factor[~match]
        caps = len(identities)
    elif method == 'slice':
        proposal = AffineSlice.from_scene(scene, receiver, resolution)
        proposal = AffineSlice(proposal.vertices.astype(np.float32))
        guide_alpha = max(alpha, minimum_guide_alpha)
        for index in range(count):
            if random.uniform() < DEFENSIVE_FRACTION:
                coordinates = random.uniform(0, 1, 2)
                density = proposal.evaluate_pdf(coordinates, guide_alpha)
            else:
                coordinates, density, _ = proposal.sample(random.normal(size=2)*guide_alpha/np.sqrt(2), random.uniform(), guide_alpha)
            if coordinates is not None:
                samples[index] = evaluate_connection(scene, receiver, color, coordinates, light_coordinates[index], alpha, eta, extinction)/density
    else:
        coordinates = random.uniform(0, 1, (count, 2))
        use_cosine = random.uniform(0, 1, count) < .5
        radius = np.sqrt(coordinates[:, 0])
        angle = 2*np.pi*coordinates[:, 1]
        directions = np.c_[radius*np.cos(angle), np.sqrt(1-coordinates[:, 0]), radius*np.sin(angle)]
        _, identities, projected = scene.intersect(receiver, directions)
        coordinates[use_cosine] = projected[use_cosine]
        valid = ~use_cosine | (identities == 1)
        position = make_point(scene.transforms[1], coordinates)
        direction = normalize(position-receiver)
        p_cosine = (np.maximum(0, direction @ scene.normals[2])/np.pi * scene.areas[1] *
                    np.maximum(0, -direction @ scene.normals[1])/np.sum((position-receiver)**2, axis=1))
        samples[valid] = evaluate_connection(scene, receiver, color, coordinates[valid], light_coordinates[valid], alpha, eta, extinction)/(.5+.5*p_cosine[valid, None])
    return samples, caps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--extent', type=int, default=32)
    parser.add_argument('--resolution', type=int, default=64)
    parser.add_argument('--samples', type=int, default=1024)
    parser.add_argument('--alpha', type=float, default=.02)
    parser.add_argument('--minimum-guide-alpha', type=float, default=.05)
    parser.add_argument('--pixel', type=int, nargs='+', default=[784, 844, 724])
    parser.add_argument('--methods', nargs='+', choices=['connection-mis', 'slice', 'sms'], default=['connection-mis', 'slice', 'sms'])
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=False)
    scene = Scene(arguments.upstream / SOURCE['directory'])
    positions, colors, mask = make_receivers(scene, arguments.extent)
    eta, extinction = read_gold_ior(arguments.upstream)
    records = []
    for pixel in arguments.pixel:
        if not mask[pixel]:
            raise ValueError(f'Pixel {pixel} is not a floor receiver; select a supported probe.')
        for method in arguments.methods:
            print('CPU probe', pixel, method, flush=True)
            samples, caps = estimate(scene, positions[pixel], colors[pixel], method, arguments.alpha, arguments.samples,
                np.random.default_rng(7719+pixel), eta, extinction, arguments.resolution, arguments.minimum_guide_alpha)
            np.save(arguments.output / f'{pixel}-{method}.npy', samples)
            records.append(dict(pixel=pixel, method=method, mean=samples.mean(axis=0).tolist(),
                standard_error=(samples.std(axis=0, ddof=1)/np.sqrt(len(samples))).tolist(),
                positive_samples=int(np.count_nonzero(samples.max(axis=1))), capped_bernoulli=caps))
            (arguments.output / 'report.json').write_text(json.dumps(dict(source=SOURCE, samples=arguments.samples,
                extent=arguments.extent, alpha=arguments.alpha, resolution=arguments.resolution,
                minimum_guide_alpha=arguments.minimum_guide_alpha, records=records), indent=2)+'\n')


if __name__ == '__main__':
    main()
