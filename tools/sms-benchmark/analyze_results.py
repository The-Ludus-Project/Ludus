#!/usr/bin/env python3
"""Analyze preserved runs against a separately generated, uncertainty-checked reference."""
import argparse
import csv
import json
from pathlib import Path

import numpy as np

from render_connections import METHODS, make_preview


def make_error_preview(output, rows, references):
    from PIL import Image, ImageDraw
    size = 256
    margin = 40
    canvas = Image.new('RGB', (len(METHODS)*size, len(references)*(size+margin)), '#15191f')
    draw = ImageDraw.Draw(canvas)
    for row_index, (alpha, reference) in enumerate(references.items()):
        scale = max(float(np.percentile(reference, 99))*.25, 1e-8)
        for column, method in enumerate(METHODS):
            row = next(value for value in rows if value['alpha'] == alpha and value['method'] == method)
            image = np.load(row['arrays'])['image']
            error = np.mean(np.abs(image-reference), axis=-1)/scale
            # Fixed display-only scale within each roughness row; metric inputs
            # and the archived linear images are never clamped or tone mapped.
            intensity = np.clip(error, 0, 1)
            color = np.stack((intensity, intensity*intensity, np.zeros_like(intensity)), axis=-1)
            bitmap = Image.fromarray(np.asarray(color*255, dtype=np.uint8)).resize((size, size), Image.Resampling.NEAREST)
            canvas.paste(bitmap, (column*size, row_index*(size+margin)+margin))
            draw.text((column*size+8, row_index*(size+margin)+8), f'alpha={alpha} {method}: absolute error', fill='white')
    canvas.save(output / 'absolute-error.png')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quadrature', type=Path)
    parser.add_argument('--cpu-probes', type=Path)
    parser.add_argument('--cpu-mis', type=Path)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=False)
    runs = json.loads((arguments.runs / 'report.json').read_text())
    reference_report = json.loads((arguments.reference / 'report.json').read_text())
    if runs['prepared']['buffer_sha256'] != reference_report['prepared']['buffer_sha256']:
        raise ValueError('The runs and reference have different represented inputs; repeat with matching fixtures.')
    rows = []
    references = {}
    uncertainties = {}
    for reference_record in reference_report['records']:
        alpha = reference_record['alpha']
        data = np.load(arguments.reference / f'reference-{alpha}.npz')
        image = data['image'].astype(np.float64)
        variance = data['variance'].astype(np.float64)
        references[alpha] = image
        uncertainty = float(np.sqrt(variance.sum()/max(np.sum(image*image), 1e-30)))
        uncertainties[alpha] = uncertainty
        for original in runs['runs']:
            if original['alpha'] != alpha:
                continue
            row = dict(original)
            row['arrays'] = str((arguments.runs / row['arrays']).resolve())
            candidate = np.load(row['arrays'])['image'].astype(np.float64)
            row['mse'] = float(np.mean((candidate-image)**2))
            row['nrmse'] = float(np.sqrt(np.sum((candidate-image)**2)/max(np.sum(image*image), 1e-30)))
            row['reference_estimated_relative_standard_error'] = uncertainty
            row['is_reference_qualified'] = uncertainty <= reference_report['reference_quality_target']
            row['mse_times_wall_seconds'] = row['mse']*row['wall_seconds']
            row['mse_times_gpu_seconds'] = row['mse']*row['gpu_seconds']
            rows.append(row)
    summaries = []
    for alpha in references:
        for method in METHODS:
            selected = [row for row in rows if row['alpha'] == alpha and row['method'] == method]
            if len(selected) == 0:
                raise ValueError(f'Missing {alpha}/{method}; finish the complete comparison before summarizing.')
            summary = dict(alpha=alpha, method=method, repeats=len(selected),
                           reference_relative_standard_error=uncertainties[alpha],
                           is_reference_qualified=all(row['is_reference_qualified'] for row in selected),
                           bernoulli_caps=sum(row['capped_bernoulli'] for row in selected),
                           maximum_time_mismatch=max(row['relative_time_mismatch'] for row in selected))
            for field in ('mse', 'nrmse', 'spp_mean', 'wall_seconds', 'gpu_seconds', 'samples_per_wall_second',
                          'mse_times_wall_seconds', 'mse_times_gpu_seconds'):
                values = [row[field] for row in selected]
                summary['median_'+field] = float(np.median(values))
                if field == 'nrmse':
                    summary['minimum_nrmse'] = min(values)
                    summary['maximum_nrmse'] = max(values)
            summaries.append(summary)
    checks = {}
    if arguments.quadrature is not None:
        quadrature = json.loads((arguments.quadrature / 'report.json').read_text())
        probes = []
        for pixel in sorted(set(row['pixel'] for row in quadrature['records'])):
            values = sorted([row for row in quadrature['records'] if row['pixel'] == pixel], key=lambda row: row['resolution'])
            if len(values) < 2:
                raise ValueError('CPU quadrature needs at least two resolutions per probe.')
            first = np.array(values[-2]['integral'])
            expected = np.array(values[-1]['integral'])
            convergence = float(np.max(np.abs(first-expected)/np.maximum(np.abs(expected), 1e-30)))
            alpha = quadrature['alpha']
            gpu_reference = references[alpha].reshape(-1, 3)[pixel]
            gpu_variance = np.load(arguments.reference / f'reference-{alpha}.npz')['variance'].reshape(-1, 3)[pixel]
            # A small deterministic quadrature-convergence allowance plus five
            # MC standard errors; this is empirical evidence, not certification.
            tolerance = .005*np.abs(expected)+5*np.sqrt(gpu_variance)
            probes.append(dict(pixel=pixel, convergence=convergence, expected=expected.tolist(),
                               gpu_reference=gpu_reference.tolist(),
                               is_gpu_reference_consistent=bool(np.all(np.abs(gpu_reference-expected) <= tolerance))))
        checks['quadrature'] = probes
        if arguments.cpu_mis is not None:
            checks['cpu_mis_pilot'] = 'The low-count MIS pilot is superseded for validation by the separate --cpu-mis report; both original reports are preserved.'
        for path, name in [(arguments.cpu_probes, 'cpu_oracle'), (arguments.cpu_mis, 'cpu_mis')]:
            if path is None:
                continue
            report = json.loads((path / 'report.json').read_text())
            results = []
            for row in report['records']:
                # The dedicated high-count run supersedes the rare-event MIS pilot.
                if name == 'cpu_oracle' and row['method'] == 'connection-mis' and arguments.cpu_mis is not None:
                    continue
                expected = np.array(next(value['expected'] for value in probes if value['pixel'] == row['pixel']))
                observed = np.array(row['mean'])
                tolerance = .005*np.abs(expected)+5*np.array(row['standard_error'])
                results.append(dict(pixel=row['pixel'], method=row['method'],
                                    is_consistent=bool(np.all(np.abs(observed-expected) <= tolerance)),
                                    capped_bernoulli=row['capped_bernoulli']))
            checks[name] = results
    report = dict(scope=runs['scope'], source=runs['source'], adapter=runs['adapter'],
                  original_run_report=str((arguments.runs / 'report.json').resolve()),
                  reference_report=str((arguments.reference / 'report.json').resolve()),
                  reference_method=reference_report['reference_method'], reference_samples=reference_report['reference_samples'],
                  preparation_seconds=runs['prepared']['preprocessing_seconds'],
                  guide_buffer_bytes=sum(runs['prepared']['buffer_bytes'][name] for name in ('vertices', 'acceleration', 'bins', 'identities')),
                  setup_seconds=runs['setup_seconds'], summaries=summaries, runs=rows, checks=checks,
                  limitations=runs['limitations']+['32x32 pixel caustic diagnostic; full original integrator renders are separate.',
                      'Empirical reference/CPU checks do not certify nonlinear root coverage or establish present-day SOTA superiority.'])
    (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    with (arguments.output / 'summary.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)
    make_preview(arguments.output, rows, references)
    make_error_preview(arguments.output, rows, references)
    print(json.dumps(dict(summaries=summaries, checks=checks), indent=2))


if __name__ == '__main__':
    main()
