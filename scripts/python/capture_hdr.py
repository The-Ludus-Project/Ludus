"""Validate and read Ludus linear HDR capture files."""
import hashlib
import math
import struct


# Render graph capture flags (modules/content/render_graph_format/include/ludus/content/render_graph_format/capture.h).
GRAPH_FLAGS = {'partial': 1, 'failed': 2, 'limit_override': 4, 'orthographic': 8, 'reverse_depth': 16, 'infinite_far': 32}
GRAPH_MAXIMUM_SAMPLES = 65536
HEADER_BYTES = 256
PIXEL = struct.Struct('<fffI')
GRAPH_MAGIC = b'LURGCAP\0'
REFERENCE_MAGIC = b'LUHDR01\0'


def read_pixels(data, maximum_samples, keep_pixels):
    """Validate sample records and optionally retain the decoded tuples."""
    minimum = maximum_samples
    maximum = 0
    pixels = [] if keep_pixels else None
    for red, green, blue, samples in PIXEL.iter_unpack(memoryview(data)[HEADER_BYTES:]):
        if not (0 <= red < math.inf and 0 <= green < math.inf and 0 <= blue < math.inf
                and samples <= maximum_samples):
            raise ValueError('Nonfinite/negative radiance or invalid count')
        minimum = min(minimum, samples)
        maximum = max(maximum, samples)
        if pixels is not None:
            pixels.append((red, green, blue, samples))
    return minimum, maximum, pixels


def decode_graph_capture(data, keep_pixels):
    """Decode a LURGCAP file: the RT1 pixel record behind a header that identifies the graph and its route."""
    version, header, pixel_bytes, flags, width, height, target, submitted, failed, output, accumulation_pass, authored = \
        struct.unpack_from('<12I', data, 8)
    if (version, header, pixel_bytes) != (1, HEADER_BYTES, PIXEL.size) or flags & ~63 or not width or not height:
        raise ValueError('Unsupported render graph capture version/header')
    if len(data) != HEADER_BYTES + width * height * PIXEL.size or not 1 <= target <= GRAPH_MAXIMUM_SAMPLES or submitted > target:
        raise ValueError('Invalid capture extent or sample counts')
    minimum, maximum, pixels = read_pixels(data, submitted, keep_pixels)
    if not flags & (GRAPH_FLAGS['partial'] | GRAPH_FLAGS['failed']) and minimum < target:
        raise ValueError('Complete capture has uncommitted pixels')
    if bool(flags & GRAPH_FLAGS['failed']) != bool(failed):
        raise ValueError('Failure flag disagrees with the failed pixel count')
    revision, = struct.unpack_from('<Q', data, 56)
    metadata = dict(kind='render_graph', schema=version, width=width, height=height, target_samples=target,
                    submitted_samples=submitted, failed_pixels=failed, output_resource=output,
                    accumulation_pass=accumulation_pass, authored_limit=authored, flags=flags,
                    **{name: bool(flags & bit) for name, bit in GRAPH_FLAGS.items()},
                    scene_revision=revision, package_identity=data[64:96].hex(), scene_identity=data[96:128].hex(),
                    minimum_samples=minimum, maximum_samples=maximum,
                    world_to_clip=list(struct.unpack_from('<16f', data, 128)),
                    camera_position=list(struct.unpack_from('<3f', data, 192)),
                    sha256=hashlib.sha256(data).hexdigest())
    return metadata, pixels


def decode(data, *, keep_pixels=True):
    """Return capture metadata and, by default, row-major (R, G, B, count) tuples."""
    if len(data) >= HEADER_BYTES and data[:8] == GRAPH_MAGIC:
        return decode_graph_capture(data, keep_pixels)
    if len(data) < HEADER_BYTES or data[:8] != REFERENCE_MAGIC:
        raise ValueError('Not a LUHDR01 capture')
    schema, profile, sampler, header, width, height, target, depth, roulette, flags = struct.unpack_from('<10I', data, 8)
    # Sampler 1 is UniformArea light sampling; flag 128 marks the resampled samplers, 2 for
    # ResampledProjectedSolidAngle and 3 for ResampledFormFactor. Sampler 4 separates directional lights.
    if (schema, header) != (1, HEADER_BYTES) or profile not in (1, 2, 3, 4, 5, 6, 7, 8) or not width or not height or flags & ~511 or (flags & 1 and flags & 64):
        raise ValueError('Unsupported capture profile/header')
    if (sampler, bool(flags & 128)) not in ((1, False), (2, True), (3, True), (4, False)):
        raise ValueError('Unsupported sampler version or light-sampling flag')
    if sampler == 4 and not flags & (1 | 64):
        raise ValueError('Separate directional sampling requires CPU oracle or GPU compute')
    if len(data) != HEADER_BYTES + width * height * PIXEL.size or not 1 <= target <= 16777215 or not 1 <= depth <= 64 or not 1 <= roulette <= 64:
        raise ValueError('Invalid capture extent or sampling limits')
    optical_wavelength = struct.unpack_from('<f', data, 92)[0]
    if not math.isfinite(optical_wavelength) or optical_wavelength < 0 or bool(flags & 256) != (optical_wavelength > 0):
        raise ValueError('Invalid optical wavelength/flag')
    if flags & 256 and (sampler != 1 or not flags & (1 | 64)):
        raise ValueError('Optical captures require UniformArea and CPU oracle or GPU compute')
    minimum, maximum, pixels = read_pixels(data, 16777215, keep_pixels)
    if not flags & 32 and minimum < target:
        raise ValueError('Complete capture has uncommitted pixels')
    seed, revision, low, high = struct.unpack_from('<4Q', data, 48)
    metadata = dict(schema=schema, profile=profile, sampler=sampler, width=width, height=height,
                    optical_wavelength_world=optical_wavelength,
                    target_samples=target, max_depth=depth, roulette_depth=roulette, flags=flags,
                    seed=seed, revision=revision, scene_fingerprint=f'{high:016x}{low:016x}',
                    traversal='cpu_oracle' if flags & 1 else 'gpu_compute' if flags & 64 else 'native_queries',
                    light_sampling={1: 'uniform_area', 2: 'resampled_projected_solid_angle',
                                    3: 'resampled_form_factor', 4: 'separate_directional'}[sampler],
                    center_rays=bool(flags & 2), partial=bool(flags & 32), minimum_samples=minimum, maximum_samples=maximum,
                    secondary_distance=struct.unpack_from('<f', data, 80)[0], texture_mip=struct.unpack_from('<I', data, 84)[0],
                    light_count=struct.unpack_from('<I', data, 88)[0], world_to_clip=list(struct.unpack_from('<16f', data, 96)),
                    camera_position=list(struct.unpack_from('<3f', data, 160)), environment=list(struct.unpack_from('<3f', data, 172)),
                    sha256=hashlib.sha256(data).hexdigest())
    return metadata, pixels
