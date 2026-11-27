"""Compare the Editor and a relocated SDK app to the projected cubes of the saved raster baseline Worlds.

cube.world.json checks a flat colour export. labels.world.json checks a categorical label export and its
matching colour export (see labels.py), under appearance-only and geometry edits and a label swap.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

import labels


FIXTURE = Path(__file__).resolve().parent
ROOT = FIXTURE.parents[2]
EXTENT = (96, 64)
# The camera sees 12 x 8 metres. The cube covers [-2,0] x [0,2] relative to
# the optical axis, so its boundaries fall between pixel centres on every side.
CUBE = (32, 16, 48, 32)  # right and bottom exclusive
# Label views: background black, the foreground orange and the rear cyan; any other label magenta.
LABEL_VIEW_COLORS = {0: (0, 0, 0), 1: (255, 128, 0), 2: (0, 192, 255)}
LABEL_VIEW_UNKNOWN = (255, 0, 255)
# Moving the foreground cube 0.5 m (four pixels) right uncovers a 4 x 16 strip of background and covers
# 4 x 16 pixels to its right: rows 16-23 become foreground over background and rows 24-31 over the rear cube.
GEOMETRY_SHIFT = 0.5
GEOMETRY_CHANGED_PIXELS = 2 * 4 * 16


def write_expectation(path, color):
    image = Image.new('RGB', EXTENT)
    image.paste(color, CUBE)
    image.save(path)


def capture(command, package, evidence, name, environment, outputs=('capture.png',)):
    """Run one host; move each output PNG to <evidence>/<name>.png, or <name>-<output> for several."""
    for output in outputs:
        (package / output).unlink(missing_ok=True)
    with (evidence / f'{name}.log').open('w') as log:
        process = subprocess.run(command, cwd=package, env=environment, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=150)
    if process.returncode or not all((package / output).is_file() for output in outputs):
        raise RuntimeError(f'{name} failed to capture. Inspect {log.name}')
    for output in outputs:
        shutil.move(package / output, evidence / (f'{name}.png' if len(outputs) == 1 else f'{name}-{output}'))


def compare(evidence, label, candidates, expected_code):
    report = evidence / f'{label}.json'
    with report.open('x') as output:
        process = subprocess.run([sys.executable, ROOT / 'scripts/compare-render', 'expected.png',
                                  *candidates, '--max-rmse', '0', '-o', f'{label}.html'],
                                 cwd=evidence, stdout=output)
    if process.returncode != expected_code:
        raise RuntimeError(f'{label}: expected exit {expected_code}, got {process.returncode}. See {report}')


def write_label_view(path, grid):
    """Draw a label grid with one fixed colour per label, for the HTML heatmap; zero stays black."""
    image = Image.new('RGB', EXTENT)
    image.putdata([LABEL_VIEW_COLORS.get(label, LABEL_VIEW_UNKNOWN) for row in grid for label in row])
    image.save(path)


def check_labels(evidence, name, grid, triangles):
    """Check <name>-labels.png against a prediction; return whether it matched and the captured grid."""
    with Image.open(evidence / f'{name}-labels.png') as image:
        if image.mode != 'RGBA':
            raise RuntimeError(f'{name}: the label export must be saved with its alpha identity')
        report, captured = labels.check(image, grid, triangles)
    (evidence / f'{name}-labels.json').write_text(json.dumps(report, indent=2) + '\n')
    write_label_view(evidence / f'{name}-label-view.png', captured)
    return report['passed'], captured


def verify_labels(routes, package, evidence, environment, label_graph):
    """Run the label baseline: positives, an appearance edit, a geometry edit and a label swap."""
    evidence = evidence / 'labels'
    evidence.mkdir()
    shutil.copyfile(label_graph, package / 'labels.lurgrt')
    shutil.copyfile(str(label_graph) + '.resources.json', package / 'labels.lurgrt.resources.json')
    shutil.copyfile(str(label_graph) + '.resources.json', evidence / 'graph.resources.json')
    world_path = package / 'labels.world.json'
    saved = json.loads((FIXTURE / world_path.name).read_text())
    mesh = labels.read_mesh(package / 'authoring/cube.mesh.json')
    commands = {'editor': routes['editor'][:1] + ['[.sdk-label-baseline]'],
                'sdk': routes['sdk'] + ['labels.world.json', 'labels.lurgrt',
                                        'Main/labels', 'labels.png', 'Main/color', 'color.png']}

    def run(world, phase):
        world_path.write_text(json.dumps(world, indent=2) + '\n')
        shutil.copyfile(world_path, evidence / f'{phase}.world.json')
        grid, triangles, colors = labels.predict(world, mesh, EXTENT)
        write_label_view(evidence / f'{phase}-expected-label-view.png', grid)
        expected = Image.new('RGB', EXTENT)
        expected.putdata([pixel for row in labels.paint_colors(grid, colors) for pixel in row])
        expected.save(evidence / f'{phase}-expected-color.png')
        names = []
        for host, command in commands.items():
            name = f'{host}-{phase}'
            capture(command, package, evidence, name, environment, outputs=('labels.png', 'color.png'))
            names.append(name)
        return grid, triangles, names

    def compare_views(phase, names, expected_view, expected_code):
        for kind, suffix in (('label', 'label-view'), ('color', 'color')):
            candidates = [f'{name}-{suffix}.png' for name in names]
            report = evidence / f'{phase}-{kind}.json'
            with report.open('x') as output:
                process = subprocess.run([sys.executable, ROOT / 'scripts/compare-render',
                                          expected_view if kind == 'label' else f'{phase}-expected-color.png',
                                          *candidates, '--max-rmse', '0', '-o', f'{phase}-{kind}.html'],
                                         cwd=evidence, stdout=output)
            if process.returncode != (expected_code if kind == 'label' else 0):
                raise RuntimeError(f'{phase} {kind}: expected exit {expected_code}, got {process.returncode}. See {report}')

    baseline_grid = None
    for phase in ('first', 'repeat'):
        baseline_grid, triangles, names = run(saved, phase)
        for name in names:
            if not check_labels(evidence, name, baseline_grid, triangles)[0]:
                raise RuntimeError(f'{name}: labels differ from the projected cubes. See {name}-labels.json')
        compare_views(phase, names, f'{phase}-expected-label-view.png', 0)

    # Appearance only: new tints, identical labels.
    tinted = json.loads(json.dumps(saved))
    for entity, rgb in zip(tinted['entities'], ((0, 1, 0), (1, 1, 0))):
        entity['components']['renderable'].update(r=rgb[0], g=rgb[1], b=rgb[2])
    grid, triangles, names = run(tinted, 'appearance')
    if grid != baseline_grid:
        raise RuntimeError('An appearance edit must not change the predicted labels')
    for name in names:
        if not check_labels(evidence, name, grid, triangles)[0]:
            raise RuntimeError(f'{name}: an appearance edit changed the labels. See {name}-labels.json')
        with Image.open(evidence / f'{name}-labels.png') as edited, \
                Image.open(evidence / f'{name.split("-")[0]}-first-labels.png') as first:
            if edited.tobytes() != first.tobytes():
                raise RuntimeError(f'{name}: label bytes changed under an appearance edit')
    compare_views('appearance', names, 'appearance-expected-label-view.png', 0)

    # Geometry: the foreground moves right by GEOMETRY_SHIFT metres; only its silhouette's sweep changes.
    moved = json.loads(json.dumps(saved))
    moved['entities'][0]['components']['transform']['x'] += GEOMETRY_SHIFT
    grid, triangles, names = run(moved, 'geometry')
    changed = sum(a != b for old, new in zip(baseline_grid, grid) for a, b in zip(old, new))
    if changed != GEOMETRY_CHANGED_PIXELS:
        raise RuntimeError(f'The geometry edit should change {GEOMETRY_CHANGED_PIXELS} labels, not {changed}')
    for name in names:
        if not check_labels(evidence, name, grid, triangles)[0]:
            raise RuntimeError(f'{name}: labels do not follow the geometry edit. See {name}-labels.json')
    compare_views('geometry', names, 'geometry-expected-label-view.png', 0)

    # Negative control: reorder the renderables, which swaps their labels, and keep the saved expectation.
    swapped = json.loads(json.dumps(saved))
    swapped['entities'][:2] = swapped['entities'][1::-1]
    swapped_grid, swapped_triangles, names = run(swapped, 'swap')
    foreground = sum(label != 0 for row in baseline_grid for label in row)
    for name in names:
        matches, captured = check_labels(evidence, name, swapped_grid, swapped_triangles)
        if not matches:
            raise RuntimeError(f'{name}: the swapped World must produce the swapped labels. See {name}-labels.json')
        differing = sum(a != b for old, new in zip(baseline_grid, captured) for a, b in zip(old, new))
        if differing != foreground:
            raise RuntimeError(f'{name}: the swap should change all {foreground} labelled pixels, not {differing}')
    compare_views('swap', names, 'first-expected-label-view.png', 1)


def verify(editor, executable, graph, evidence, driver, label_graph):
    evidence.mkdir(parents=True, exist_ok=True)
    environment = {'PATH': '/usr/bin:/bin', 'QT_QPA_PLATFORM': 'offscreen',
                   'VK_DRIVER_FILES': str(driver), 'LANG': 'C.UTF-8'}
    if 'GALLIVM_PERF' in os.environ:
        environment['GALLIVM_PERF'] = os.environ['GALLIVM_PERF']
    version = subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', 'mesa-vulkan-drivers'], text=True)
    write_expectation(evidence / 'expected.png', 'red')
    with tempfile.TemporaryDirectory() as workspace:
        package = Path(workspace) / 'moved' / 'app'
        authoring = package / 'authoring'
        authoring.mkdir(parents=True)
        # The installed executable and its packaged libraries travel together.
        shutil.copytree(executable.parent, package, dirs_exist_ok=True)
        linked = subprocess.check_output(['ldd', './' + executable.name], cwd=package, env=environment, text=True)
        (evidence / 'sdk-libraries.txt').write_text(linked)
        for line in linked.splitlines():
            if 'not found' in line:
                raise RuntimeError(f'Missing packaged SDK dependency: {line}')
            if ' => /' in line:
                library = Path(line.split(' => ', 1)[1].split(' (', 1)[0]).resolve()
                if not any(library.is_relative_to(root) for root in (package, Path('/usr/lib'), Path('/lib'))):
                    raise RuntimeError(f'SDK runtime dependency outside package/system libraries: {library}')
        shutil.copyfile(graph, package / 'graph.lurgrt')
        names = Path(str(graph) + '.resources.json')
        shutil.copyfile(names, package / 'graph.lurgrt.resources.json')
        shutil.copyfile(names, evidence / 'graph.resources.json')
        for asset in ('cube.mesh.json', 'world.schema.json', 'render.preview.json', 'ludus.png'):
            shutil.copyfile(ROOT / 'examples/live-edit-game/authoring' / asset, authoring / asset)
        world_path = package / 'cube.world.json'
        shutil.copyfile(FIXTURE / world_path.name, world_path)
        metadata = {'driver_package': 'mesa-vulkan-drivers', 'driver_version': version,
                    'driver_manifest': json.loads(driver.read_text()), 'export': 'Main/output',
                    'camera': 'preview.camera', 'extent': EXTENT, 'configuration': 'Default',
                    'runtime_sha256': hashlib.sha256(graph.read_bytes()).hexdigest(),
                    'world_sha256': hashlib.sha256(world_path.read_bytes()).hexdigest(),
                    'max_rmse': 0, 'encoding': 'normalized 8-bit display RGB'}
        (evidence / 'fixture.json').write_text(json.dumps(metadata, indent=2) + '\n')
        routes = {'editor': [editor, '[.sdk-raster-baseline]'], 'sdk': ['./' + executable.name]}
        positives = []
        for attempt in ('first', 'repeat'):
            for host, command in routes.items():
                name = f'{host}-{attempt}'
                capture(command, package, evidence, name, environment)
                positives.append(f'{name}.png')
        compare(evidence, 'baseline', positives, 0)

        # Change the material in the saved World. Both processes reopen the file.
        world = json.loads(world_path.read_bytes())
        cube = next(entity for entity in world['entities'] if entity['id'] == 'starter.cube')
        cube['components']['renderable'].update(r=0, g=1)
        world_path.write_text(json.dumps(world, indent=2) + '\n')
        shutil.copyfile(world_path, evidence / 'green.world.json')
        for host, command in routes.items():
            capture(command, package, evidence, f'{host}-green', environment)
        compare(evidence, 'deliberate-failure', ['editor-green.png', 'sdk-green.png'], 1)
        report = json.loads((evidence / 'deliberate-failure.json').read_bytes())
        # A crash or an arbitrary wrong image is not the negative control.
        expected_rmse = math.sqrt(2 * 16 * 16 / (3 * 96 * 64))
        for comparison in report['comparisons']:
            if comparison['regression']['passed'] or not math.isclose(comparison['metrics']['rmse'], expected_rmse):
                raise RuntimeError('The deliberate color change did not produce its predicted error')
        green = Image.new('RGB', EXTENT)
        green.paste((0, 255, 0), CUBE)
        for host in routes:
            with Image.open(evidence / f'{host}-green.png') as result:
                if result.convert('RGB').tobytes() != green.tobytes():
                    raise RuntimeError(f'{host}: the changed pixels must be the green cube')
        verify_labels(routes, package, evidence, environment, label_graph)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('editor', 'executable', 'graph', 'evidence', 'driver', 'label-graph'):
        parser.add_argument('--' + option, type=Path, required=True)
    verify(**{name: path.resolve() for name, path in vars(parser.parse_args()).items()})
