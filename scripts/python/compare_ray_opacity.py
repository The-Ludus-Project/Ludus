#!/usr/bin/env python3
"""Compare native [opacity] benchmark artifacts from saved before/after executables.

Run alternating before/after measurements on the same device with identical fixture
settings. GPU intervals cover compute plus output raster; CPU setup and completed
frame wall time are separate. Compare final RGBA8 output and raw linear RGB when
both executables provide it; neither comparison establishes statistical convergence.
"""
import argparse
from array import array
import math
import json
from pathlib import Path
import statistics
import struct
import sys

SCENES = ('empty', 'opaque', 'masked', 'mixed')
PIXEL_BYTES = 512 * 512 * 4
TIMING_SAMPLES = 48 - 8


def read_values(directory, scene, suffix, encoding, count):
    data = (directory / f'{scene}.{suffix}.bin').read_bytes()
    expected = struct.calcsize(encoding) * count
    if len(data) != expected:
        raise ValueError(f'{directory}/{scene}.{suffix}.bin: expected {expected} bytes, got {len(data)}')
    return struct.unpack('<' + encoding * count, data)


def summarize(directories, scene, suffix, encoding, count):
    runs = [read_values(directory, scene, suffix, encoding, count) for directory in directories]
    return {
        'median_ms': statistics.median(value for run in runs for value in run) / 1e6,
        'run_medians_ms': [statistics.median(run) / 1e6 for run in runs],
    }


def compare(before, after):
    if len(before) != len(after):
        raise ValueError('Supply the same number of before and after runs')
    results = {}
    for scene in SCENES:
        maximum_difference = 0
        changed_channels = 0
        for original, changed in zip(before, after):
            first = (original / f'{scene}.rgba').read_bytes()
            second = (changed / f'{scene}.rgba').read_bytes()
            if len(first) != PIXEL_BYTES or len(second) != PIXEL_BYTES:
                raise ValueError(f'{scene}: images must contain {PIXEL_BYTES} RGBA8 bytes')
            differences = [abs(a - b) for a, b in zip(first, second)]
            maximum_difference = max(maximum_difference, max(differences))
            changed_channels += sum(difference != 0 for difference in differences)
        entry = {
            'maximum_channel_difference': maximum_difference,
            'changed_channels': changed_channels,
        }
        linear_before = [directory / f'{scene}.linear.bin' for directory in before]
        linear_after = [directory / f'{scene}.linear.bin' for directory in after]
        if all(path.exists() for path in linear_before + linear_after):
            squared_error = 0.0
            signed_error = 0.0
            maximum_linear_error = 0.0
            channels = 0
            for first_path, second_path in zip(linear_before, linear_after):
                first_values = array('f')
                second_values = array('f')
                first_values.frombytes(first_path.read_bytes())
                second_values.frombytes(second_path.read_bytes())
                if sys.byteorder != 'little':
                    first_values.byteswap()
                    second_values.byteswap()
                if len(first_values) != PIXEL_BYTES * 2 or len(second_values) != len(first_values):
                    raise ValueError(f'{scene}: invalid raw accumulation extent')
                for pixel in range(PIXEL_BYTES // 4):
                    for channel in range(3):
                        first_value = first_values[pixel * 8 + channel]
                        second_value = second_values[pixel * 8 + channel]
                        if not math.isfinite(first_value) or not math.isfinite(second_value):
                            raise ValueError(f'{scene}: nonfinite raw RGB at pixel {pixel}, channel {channel}')
                        difference = second_value - first_value
                        squared_error += difference * difference
                        signed_error += difference
                        maximum_linear_error = max(maximum_linear_error, abs(difference))
                        channels += 1
            entry['linear'] = {
                'maximum_absolute_error': maximum_linear_error,
                'rmse': math.sqrt(squared_error / channels),
                'mean_signed_error': signed_error / channels,
            }
        for suffix, encoding, count in (('gpu', 'd', TIMING_SAMPLES), ('wall', 'Q', TIMING_SAMPLES), ('setup', 'Q', 1)):
            first = summarize(before, scene, suffix, encoding, count)
            second = summarize(after, scene, suffix, encoding, count)
            entry[suffix] = {
                'before': first,
                'after': second,
                'speedup': first['median_ms'] / second['median_ms'] if second['median_ms'] else None,
            }
        results[scene] = entry
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', nargs='+', type=Path, required=True)
    parser.add_argument('--after', nargs='+', type=Path, required=True)
    options = parser.parse_args()
    results = compare(options.before, options.after)
    print(json.dumps(results, indent=2))
    return int(any(result['changed_channels'] != 0 for result in results.values()))


if __name__ == '__main__':
    raise SystemExit(main())
