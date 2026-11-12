"""Deterministic bounded raw-texel mip cooker; no image codec or runtime compiler.

Input JSON: width, height, format (R8Unorm/Rg8Unorm/Rgba8Unorm/Rgba8Srgb),
semantic (color/data/normal/mask), pixels (top-down tightly packed integer bytes).
Optional mask cutoff (0,1] preserves nearest attainable threshold coverage per mip.
Output: portable immutable upload header and inspectable source/level manifest.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

# Thanks to Khronos, "KTX File Format Specification", 2.0, section 3.10.2
# "Use of Transfer Functions": decode -> scale -> encode for sRGB mips.
# https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html#_use_of_transfer_functions
# This owns an uncompressed raw-texel profile, not a KTX reader/transcoder.
def decode(value):
    v = value / 255
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def encode(value):
    return quantize((12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055) * 255)


def quantize(value):
    return max(0, min(255, math.floor(value + 0.5)))


def validate_bake(document):
    bake = document.get('bake')
    if bake is None:
        return None
    if not isinstance(bake, dict) or document.get('format') != 'Rgba8Unorm' or document.get('semantic') != 'data' or document.get('mip_levels') != 1:
        raise ValueError('Bake chart requires linear RGBA8 data with one clamped mip')
    for key in ('source_id', 'scene_revision', 'dependency_revision'):
        value = bake.get(key)
        if type(value) is not int or not 1 <= value < 2 ** 64:
            raise ValueError('Bake identities/revisions must be nonzero uint64')
    direct = bake.get('direct_light_id', 0)
    if type(direct) is not int or not 0 <= direct < 2 ** 64:
        raise ValueError('Baked direct light identity must be uint64')
    for key, count in (('transform', 12), ('scale', 3)):
        values = bake.get(key)
        if not isinstance(values, list) or len(values) != count or any(type(v) not in (int, float) or abs(v) > 1000000 or not math.isfinite(v) or (v != 0 and abs(v) < 2 ** -149) for v in values):
            raise ValueError('Bake transform/scale must be bounded representable numeric arrays')
        if key == 'scale' and any(not 0 <= v <= 10000 for v in values):
            raise ValueError('Bake scale must be 0..10000')
    width, height = document['width'], document['height']
    gutter = bake.get('gutter', 1)
    if type(gutter) is not int or not 1 <= gutter <= 16 or width < 2 * gutter + 2 or height < 2 * gutter + 2:
        raise ValueError('Bake chart needs a nonempty interior and 1..16 replicated gutter texels')
    pixels = document['pixels']
    # Thanks to Marco Tarini, Cem Yuksel and Sylvain Lefebvre, "Rethinking Texture
    # Mapping", SIGGRAPH '17 Courses, section 4.3 (https://doi.org/10.1145/3084873.3084911),
    # for treating chart borders as part of the filtering footprint. This cooker
    # checks nearest-edge replication for one chart at mip zero.
    texel_bytes = 4
    row_bytes = width * texel_bytes
    gutter_bytes = gutter * texel_bytes
    right_interior_end = row_bytes - gutter_bytes
    gutter_error = 'Bake gutters must replicate the nearest chart-interior texel'
    for y in range(height):
        row = y * row_bytes
        interior_row = min(max(y, gutter), height - gutter - 1) * row_bytes
        left_texel = interior_row + gutter_bytes
        right_end = interior_row + right_interior_end
        if (
            pixels[row:row + gutter_bytes] != pixels[left_texel:left_texel + texel_bytes] * gutter
            or pixels[row + right_interior_end:row + row_bytes] != pixels[right_end - texel_bytes:right_end] * gutter
        ):
            raise ValueError(gutter_error)
        if row != interior_row and pixels[row + gutter_bytes:row + right_interior_end] != pixels[left_texel:right_end]:
            raise ValueError(gutter_error)
    return dict(bake, direct_light_id=direct, gutter=gutter,
                uv_transform=[(width - 2 * gutter - 1) / width, (height - 2 * gutter - 1) / height,
                              (gutter + 0.5) / width, (gutter + 0.5) / height])


def cook(document):
    blob, levels, _ = _cook(document)
    return blob, levels


def _cook(document):
    formats = {'R8Unorm': 1, 'Rg8Unorm': 2, 'Rgba8Unorm': 4, 'Rgba8Srgb': 4}
    width, height, fmt = document.get('width'), document.get('height'), document.get('format')
    semantic, pixels = document.get('semantic'), document.get('pixels')
    # Offline bound matches the sixteen-mip spatial envelope, not a device guarantee.
    # Runtime format/device admission and the existing upload-byte budget remain mandatory.
    if type(width) is not int or type(height) is not int or not 1 <= width <= 32768 or not 1 <= height <= 32768:
        raise ValueError('Texture dimensions must be integer 1..32768')
    if fmt not in formats or semantic not in ('color', 'data', 'normal', 'mask'):
        raise ValueError('Unknown portable format/semantic')
    channels = formats[fmt]
    maximum_levels = max(width, height).bit_length()
    requested_levels = document.get("mip_levels", maximum_levels)
    if type(requested_levels) is not int or not 1 <= requested_levels <= maximum_levels:
        raise ValueError("mip_levels must be within the complete initialized image chain")
    probe_width, probe_height, footprint, probe_level = width, height, 0, 0
    while True:
        footprint += (probe_width * channels + 3) // 4 * 4 * probe_height
        probe_level += 1
        if probe_level == requested_levels:
            break
        probe_width, probe_height = max(1, probe_width // 2), max(1, probe_height // 2)
    upload_budget = document.get("max_upload_bytes", 64 * 1024 * 1024)
    if type(upload_budget) is not int or not 1 <= upload_budget <= 512 * 1024 * 1024:
        raise ValueError("max_upload_bytes must be an integer in 1..512 MiB")
    if footprint > upload_budget:
        raise ValueError("Complete mip upload exceeds max_upload_bytes (default 64 MiB)")
    if (fmt == 'Rgba8Srgb' and semantic != 'color') or (semantic in ('color', 'normal') and channels != 4):
        raise ValueError('sRGB is color only; color/normal require RGBA8')
    if not isinstance(pixels, list) or len(pixels) != width * height * channels or any(type(v) is not int or not 0 <= v <= 255 for v in pixels):
        raise ValueError('Source must contain complete tightly packed byte texels')
    bake = validate_bake(document)
    cutoff = document.get('cutoff')
    if cutoff is not None and (semantic != 'mask' or channels not in (1, 4) or type(cutoff) not in (int, float) or not math.isfinite(cutoff) or not 0 < cutoff <= 1):
        raise ValueError('Coverage cutoff requires R8/RGBA8 mask and finite (0,1] threshold')
    alpha = channels - 1
    target = sum(pixels[i] / 255 >= cutoff for i in range(alpha, len(pixels), channels)) / (width * height) if cutoff is not None else 0
    blob, levels = bytearray(), []
    while True:
        pitch = (width * channels + 3) // 4 * 4
        level = {'width': width, 'height': height, 'offset': len(blob), 'row_pitch': pitch}
        for row in range(height):
            blob.extend(pixels[row * width * channels:(row + 1) * width * channels])
            blob.extend(bytes(pitch - width * channels))
        levels.append(level)
        if len(levels) == requested_levels:
            break
        next_width, next_height = max(1, width // 2), max(1, height // 2)
        reduced = []
        # Exact box footprints include fractional odd edges with equal output area.
        for y in range(next_height):
            for x in range(next_width):
                left, right = x * width / next_width, (x + 1) * width / next_width
                top, bottom = y * height / next_height, (y + 1) * height / next_height
                samples = [(pixels[(sy * width + sx) * channels:(sy * width + sx + 1) * channels],
                            (min(right, sx + 1) - max(left, sx)) * (min(bottom, sy + 1) - max(top, sy)))
                           for sy in range(math.floor(top), math.ceil(bottom))
                           for sx in range(math.floor(left), math.ceil(right))]
                area = sum(weight for _, weight in samples)
                alpha_weight = sum(p[3] / 255 * weight for p, weight in samples) if semantic == 'color' else 0
                value = []
                for channel in range(channels):
                    if semantic == 'color' and channel < 3:
                        # Filter associated color to avoid transparent RGB bleeding;
                        # the output remains straight alpha for the material contract.
                        average = sum((decode(p[channel]) if fmt == 'Rgba8Srgb' else p[channel] / 255) * p[3] / 255 * weight
                                      for p, weight in samples) / alpha_weight if alpha_weight > 0 else 0
                    else:
                        average = sum(p[channel] / 255 * weight for p, weight in samples) / area
                    value.append(encode(average) if fmt == 'Rgba8Srgb' and channel < 3 else quantize(average * 255))
                if semantic == 'normal':
                    normal = [sum((p[c] / 127.5 - 1) * weight for p, weight in samples) / area for c in range(3)]
                    length = math.sqrt(sum(v * v for v in normal))
                    normal = [v / length for v in normal] if length > 1e-12 else [0, 0, 1]
                    value[:3] = [quantize((v + 1) * 127.5) for v in normal]
                reduced.extend(value)
        if cutoff is not None:
            # Discrete coverage cannot always equal the parent (e.g. one texel).
            # Choose the nearest count attainable by a single monotone alpha scale.
            values = reduced[alpha::channels]
            scales = [0, 1] + [(math.ceil(cutoff * 255) - 0.5 + 1e-9) / v for v in set(values) if v > 0]
            def score(scale):
                coverage = sum(quantize(v * scale) / 255 >= cutoff for v in values) / len(values)
                return abs(coverage - target), abs(scale - 1), scale
            scale = min(scales, key=score)
            for i in range(alpha, len(reduced), channels):
                reduced[i] = quantize(reduced[i] * scale)
            level['next_coverage_error'] = score(scale)[0]
        width, height, pixels = next_width, next_height, reduced
    return bytes(blob), levels, bake


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', args.name) is None:
        raise ValueError('Texture name must be a C++ identifier')
    source_path = Path(args.source)
    if source_path.stat().st_size > 512 * 1024 * 1024:
        raise ValueError('Source JSON exceeds bounded cooker profile')
    source = source_path.read_bytes()
    document = json.loads(source)
    blob, levels, bake = _cook(document)
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    prefix = '#pragma once\n#include <ludus/graphics/rhi/raster.h>\n'
    if bake is not None: prefix += '#include <ludus/graphics/renderer/lighting.hpp>\n'
    prefix += f'namespace ludus::textures::{args.name} {{\n'
    prefix += 'inline constexpr foundation::uint8 BYTES[] = {' + ','.join(map(str, blob)) + '};\n'
    prefix += 'inline constexpr graphics::rhi::TextureMipUpload MIPS[] = {' + ','.join('{' + f'{v["offset"]},{v["row_pitch"]}' + '}' for v in levels) + '};\n'
    prefix += 'inline graphics::rhi::TextureDescription Description() noexcept { return {' + f'{document["width"]},{document["height"]},graphics::rhi::RasterFormat::{document["format"]},false,{len(levels)},true' + '}; }\n'
    prefix += 'inline graphics::rhi::TextureUpload Upload() noexcept { return {BYTES,0,MIPS}; }\n}\n'
    if bake is not None:
        def vector(values):
            return '{' + ','.join(format(float(v), '.9g') + ('F' if '.' in format(float(v), '.9g') or 'e' in format(float(v), '.9g') else '.0F') for v in values) + '}'
        transform = bake['transform']
        columns = '{' + ','.join(vector(transform[i:i + 3]) for i in range(0, 9, 3)) + '}'
        body = 'inline graphics::renderer::BakedLighting Bake(graphics::rhi::TextureHandle texture) noexcept { graphics::renderer::BakedLighting result; result.Texture=texture;'
        body += 'result.Scale=' + vector(bake['scale']) + ';result.UvTransform=' + vector(bake['uv_transform']) + ';'
        for field, key in [('SourceId','source_id'),('SceneRevision','scene_revision'),('DependencyRevision','dependency_revision'),('DirectLightId','direct_light_id')]:
            body += f'result.{field}={bake[key]}ULL;'
        body += 'result.Transform={' + columns + ',' + vector(transform[9:]) + '};return result;}\n'
        prefix = prefix[:-2] + body + '}\n'
    (out / f'{args.name}.h').write_text(prefix)
    (out / 'manifest.json').write_text(json.dumps({'source_sha256': hashlib.sha256(source).hexdigest(), 'upload_sha256': hashlib.sha256(blob).hexdigest(), 'semantic': document['semantic'], 'format': document['format'], 'levels': levels, 'bake': bake}, indent=2) + '\n')


if __name__ == '__main__':
    main()
