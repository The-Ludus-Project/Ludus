"""Headless verification driver; source compilation uses Ludus's pinned recipe."""
from pathlib import Path
import shutil
import sys

import numpy as np

from benchmark import OWNER, ROOT
from scene import normalize, read_gold_ior
from sms import prepare_smoothing
import layout

sys.path.insert(0, str(ROOT / 'scripts/python'))
import lusl_compute  # noqa: E402


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


def compile_entry(output, compiler, luslc, source_name, entry, includes=()):
    """Lower the LUSL source ``source_name`` and compile ``entry`` to WGSL.

    ``includes`` are the Slang files the sources name with ``extern slang`` beyond FoundationMath.
    Returns the WGSL text and the binding slots keyed by constant name (lusl_compute.py).
    """
    source_directory = output / 'source'
    source_directory.mkdir(parents=True)
    for source in [ROOT / 'modules/foundation/math/shaders/math.luslh',
                   ROOT / 'modules/foundation/math/shaders/math.slang',
                   *sorted((OWNER / 'shaders').glob('*.lusl*')), *includes]:
        shutil.copyfile(source, source_directory / source.name)
    layout.write_shader_layout(source_directory)
    return lusl_compute.compile_compute(lusl_compute.resolve_luslc(luslc), compiler, source_directory,
                                        source_name, entry, output / 'compile')
