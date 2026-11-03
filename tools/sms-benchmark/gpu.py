"""Headless verification driver; source compilation uses Ludus's pinned recipe."""
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np

from benchmark import OWNER, ROOT
from scene import normalize, read_gold_ior
from sms import prepare_smoothing
import layout


def pack_scene(scene, upstream):
    eta, extinction = read_gold_ior(upstream)
    records = np.zeros((layout.SCENE_RECORD_COUNT, layout.RECORD_COMPONENT_COUNT), dtype=np.float32)
    records[layout.IOR_ETA_RECORD, :3] = eta
    records[layout.IOR_EXTINCTION_RECORD, :3] = extinction
    records[layout.BASE_TANGENT_RECORD:layout.INVERSE_TRANSFORM_RECORD, :3] = normalize(scene.transforms[1, :3, :3].T)
    records[layout.BASE_TANGENT_RECORD, 3] = scene.slopes.shape[0]
    records[layout.INVERSE_TRANSFORM_RECORD:layout.REFLECTOR_CENTER_RECORD] = scene.inverse_transforms[:, :3].reshape(-1, layout.RECORD_COMPONENT_COUNT)
    records[layout.REFLECTOR_CENTER_RECORD, :3] = scene.transforms[1, :3, 3]
    records[layout.REFLECTOR_DERIVATIVE_U_RECORD:layout.LIGHT_CENTER_RECORD, :3] = 2*scene.transforms[1, :3, :2].T
    records[layout.LIGHT_CENTER_RECORD, :3] = scene.transforms[0, :3, 3]
    records[layout.LIGHT_DERIVATIVE_U_RECORD:layout.LIGHT_NORMAL_AREA_RECORD, :3] = 2*scene.transforms[0, :3, :2].T
    records[layout.LIGHT_NORMAL_AREA_RECORD, :3] = scene.normals[0]
    records[layout.LIGHT_NORMAL_AREA_RECORD, 3] = scene.areas[0]
    records[layout.FLOOR_NORMAL_RECORD, :3] = scene.normals[2]
    records[layout.AREA_RADIANCE_RECORD, :2] = [scene.areas[1], scene.radiance]
    mean, cholesky = prepare_smoothing(scene)
    records[layout.SMOOTH_SLOPE_RECORD, :2] = mean
    records[layout.SMOOTH_COVARIANCE_RECORD, :3] = [cholesky[0, 0], cholesky[1, 0], cholesky[1, 1]]
    slopes = np.zeros(scene.slopes.shape[:2]+(layout.RECORD_COMPONENT_COUNT,), dtype=np.float32)
    slopes[..., :2] = scene.slopes
    return records, slopes


def compile_entry(output, compiler, source_name, entry):
    source_directory = output / 'source'
    source_directory.mkdir(parents=True)
    for source in [ROOT / 'modules/foundation/math/shaders/math.slang',
                   ROOT / 'modules/graphics/rhi/shaders/raster_bindings.slang',
                   *sorted((OWNER / 'shaders').glob('*.slang'))]:
        shutil.copyfile(source, source_directory / source.name)
    layout.write_shader_layout(source_directory)
    subprocess.run([sys.executable, str(ROOT / 'cmake/shaders/compile_shader.py'),
                    '--source', str(source_directory / source_name), '--output', str(output / 'compile'),
                    '--name', entry, '--compiler', str(compiler),
                    '--lock', str(ROOT / 'config/shader_toolchain.json'),
                    '--graph-stage', 'compute', '--graph-target', 'wgsl'], check=True)
    return (output / 'compile/artifact.wgsl').read_text()
