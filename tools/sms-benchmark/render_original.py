#!/usr/bin/env python3
"""Run the actual authors' scalar integrators, preserving the Figure 17 scene.

Thanks to Zeltner, Georgiev and Jakob, SIGGRAPH 2020, Figure 17, and the released
render.py. The two-stage option and roughness values follow that script; extent,
seed, CPU thread count and time budget are explicit experimental overrides.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

import numpy as np

from scene import SOURCE, verify_assets


def make_run_scene(original, destination, method, extent, seed):
    root = ET.parse(original / f'plane_{method}.xml').getroot()
    film = root.find('sensor/film')
    for name in ('width', 'height'):
        film.find(f"integer[@name='{name}']").set('value', str(extent))
    sampler = root.find('sensor/sampler')
    ET.SubElement(sampler, 'integer', name='seed', value=str(seed))
    # Keep textures referenced by absolute paths; no symlinks or file copies
    # can accidentally select another task's edited scene.
    for node in root.iter('string'):
        if node.get('name') == 'filename':
            node.set('value', str((original / node.get('value')).resolve()))
    ET.ElementTree(root).write(destination, encoding='unicode')


def read_image(path):
    import OpenEXR
    channels = OpenEXR.File(str(path)).channels()
    key = 'RGB' if 'RGB' in channels else 'RGBA'
    values = channels[key].pixels[..., :3].astype(np.float32)
    if not np.all(np.isfinite(values)):
        raise ValueError(f'Nonfinite reference image: {path}; inspect its render log.')
    return values


def make_preview(output, rows):
    from PIL import Image, ImageDraw
    extent = rows[0]['extent']
    display_extent = max(extent, 256)
    margin = 32
    canvas = Image.new('RGB', (2*display_extent, len(SOURCE['roughness'])*(display_extent+margin)), '#15191f')
    draw = ImageDraw.Draw(canvas)
    images = [read_image(output / row['image']) for row in rows]
    # One exposure across every method and roughness. Original linear EXRs are
    # retained; this simple display transform is never used for error metrics.
    exposure = 1/max(float(np.percentile(np.concatenate([image.ravel() for image in images]), 99)), 1e-8)
    for record, image in zip(rows, images):
        column = 0 if record['method'] == 'sms' else 1
        row = SOURCE['roughness'].index(record['alpha'])
        values = np.maximum(image*exposure, 0)
        values = values/(1+values)
        values = np.where(values <= .0031308, 12.92*values, 1.055*values**(1/2.4)-.055)
        bitmap = Image.fromarray(np.asarray(np.clip(values*255, 0, 255), dtype=np.uint8)).resize((display_extent, display_extent))
        canvas.paste(bitmap, (column*display_extent, row*(display_extent+margin)+margin))
        draw.text((column*display_extent+8, row*(display_extent+margin)+8),
                  f'Original {record["method"].upper()} alpha={record["alpha"]}', fill='white')
    canvas.save(output / 'original-preview.png')
    return exposure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--mitsuba', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--extent', type=int, default=128)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--samples', type=int, default=999999)
    parser.add_argument('--seed', type=int, default=871)
    parser.add_argument('--threads', type=int, default=4)
    arguments = parser.parse_args()
    if min(arguments.extent, arguments.seconds, arguments.samples, arguments.threads) <= 0:
        parser.error('Extent, seconds, samples and threads must be positive.')
    original = arguments.original.resolve()
    verify_assets(original)
    arguments.output.mkdir(parents=True, exist_ok=False)
    rows = []
    environment = dict(os.environ)
    environment['PATH'] = str(arguments.mitsuba.resolve().parent)+os.pathsep+environment.get('PATH', '')
    for alpha in SOURCE['roughness']:
        for method in ('sms', 'pt'):
            stem = f'{method}-alpha-{alpha}'
            scene = arguments.output / f'{stem}.xml'
            make_run_scene(original, scene, method, arguments.extent, arguments.seed)
            command = [str(arguments.mitsuba.resolve()), '-m', 'scalar_rgb', '-t', str(arguments.threads),
                       str(scene.resolve()), '-Dsamples_per_pass=1', f'-Dspp={arguments.samples}',
                       f'-Dtimeout={arguments.seconds}', f'-Dalpha={alpha}', '-o', str((arguments.output / f'{stem}.exr').resolve())]
            if method == 'sms':
                command.append('-Dcaustics_twostage=true')
            print('Rendering', stem, flush=True)
            beginning = time.perf_counter()
            with (arguments.output / f'{stem}.log').open('w') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=environment)
            row = dict(method=method, alpha=alpha, extent=arguments.extent,
                       timeout_seconds=arguments.seconds, process_wall_seconds=time.perf_counter()-beginning,
                       command=command, exit_code=result.returncode, image=f'{stem}.exr')
            rows.append(row)
            report = dict(source=SOURCE, binary_sha256=hashlib.sha256(arguments.mitsuba.read_bytes()).hexdigest(),
                          scope='Original CPU SMS/PT reproduction; no slice sampling comparison yet.',
                          overrides=dict(extent=arguments.extent, seed=arguments.seed, threads=arguments.threads,
                                         seconds=arguments.seconds, samples=arguments.samples), runs=rows)
            (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
            if result.returncode != 0:
                raise ValueError(f'Reference render failed; inspect {stem}.log before retrying in a fresh output directory.')
            read_image(arguments.output / row['image'])
    report['preview_exposure'] = make_preview(arguments.output, rows)
    (arguments.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
