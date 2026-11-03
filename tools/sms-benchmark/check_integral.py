#!/usr/bin/env python3
"""Independent CPU tensor quadrature of selected caustic connection integrals.

This reference uses neither the slice proposal nor Newton/SMS roots. Compare
successive UV resolutions and light quadrature orders before accepting error
metrics. It is a probe check, not a certified bound for the entire image.
"""
import argparse
import json
from pathlib import Path
import time

import numpy as np

from connection import evaluate_connection, make_receivers
from scene import SOURCE, Scene, read_gold_ior


def integrate(scene, receiver, color, alpha, eta, extinction, resolution, light_order):
    nodes, weights = np.polynomial.legendre.leggauss(2)
    offsets = (nodes+1)/2
    light_nodes, light_weights = np.polynomial.legendre.leggauss(light_order)
    light_nodes = (light_nodes+1)/2
    total = np.zeros(3)
    for row in range(resolution):
        columns = np.arange(resolution)
        for v, weight_v in zip(offsets, weights):
            for u, weight_u in zip(offsets, weights):
                coordinates = np.c_[(columns+u)/resolution, np.full(resolution, (row+v)/resolution)]
                subtotal = np.zeros((resolution, 3))
                for light_row, light_v in enumerate(light_nodes):
                    for light_column, light_u in enumerate(light_nodes):
                        subtotal += evaluate_connection(scene, receiver, color, coordinates,
                            np.broadcast_to([light_u, light_v], (resolution, 2)), alpha, eta, extinction)*light_weights[light_row]*light_weights[light_column]/4
                total += subtotal.sum(axis=0)*weight_u*weight_v/(4*resolution*resolution)
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--extent', type=int, default=32)
    parser.add_argument('--pixel', type=int, nargs='+', default=[784, 844, 724])
    parser.add_argument('--resolution', type=int, nargs='+', default=[512, 1024])
    parser.add_argument('--alpha', type=float, default=.02)
    parser.add_argument('--light-order', type=int, default=2)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=False)
    scene = Scene(arguments.upstream / SOURCE['directory'])
    eta, extinction = read_gold_ior(arguments.upstream)
    positions, colors, mask = make_receivers(scene, arguments.extent)
    records = []
    for pixel in arguments.pixel:
        if not mask[pixel]:
            raise ValueError(f'Pixel {pixel} is not a floor receiver.')
        for resolution in arguments.resolution:
            print('CPU quadrature', pixel, resolution, flush=True)
            beginning = time.perf_counter()
            value = integrate(scene, positions[pixel], colors[pixel], arguments.alpha, eta, extinction, resolution, arguments.light_order)
            records.append(dict(pixel=pixel, resolution=resolution, integral=value.tolist(), seconds=time.perf_counter()-beginning))
            report = dict(source=SOURCE, alpha=arguments.alpha, extent=arguments.extent,
                          light_order=arguments.light_order, records=records,
                          scope='deterministic CPU quadrature; empirical resolution convergence, no certified global error bound')
            (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
            print(records[-1], flush=True)


if __name__ == '__main__':
    main()
