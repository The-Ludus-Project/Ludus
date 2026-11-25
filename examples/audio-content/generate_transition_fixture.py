"""Generate the two-section C6 SDK timing fixture at the native session rate."""
import json
import math
from pathlib import Path
import struct
import sys
import wave

RATE = 48000
# At 120 quarter notes/minute one 4/4 bar lasts two seconds.
BAR = RATE * 2
MARKER_OFFSET = 384
NOTES = ((220.0, 261.63, 329.63, 392.0), (293.66, 349.23, 440.0, 523.25))


def generate(root):
    root.mkdir(parents=True, exist_ok=True)
    sections = []
    resources = []
    for index, name in enumerate(("explore", "arrive")):
        # The right channel carries only a single marker in the incoming intro.
        # Both sections loop over the following bar, so playback never repeats it.
        samples = bytearray()
        for frame in range(2 * BAR):
            beat, offset = divmod(frame % BAR, BAR // 4)
            frequency = NOTES[index][beat]
            envelope = min(1.0, offset / 240, (BAR // 4 - offset) / 240)
            left = round(9000 * envelope * math.sin(2 * math.pi * frequency * offset / RATE))
            right = 24000 if index == 1 and frame == MARKER_OFFSET else 0
            samples.extend(struct.pack("<hh", left, right))
        with wave.open(str(root / f"{name}.wav"), "wb") as output:
            output.setparams((2, 2, RATE, 2 * BAR, "NONE", "not compressed"))
            output.writeframes(samples)
        source = f"source/{name}"
        resources.append({"id": source, "kind": "audio-source", "path": f"{name}.wav"})
        sections.append({"id": name, "loop_begin_frame": BAR, "loop_end_frame": 2 * BAR,
                         "layers": [{"source": source, "gain_curve": [[0, 1], [1, 1]]},
                                    {"source": source, "gain_curve": [[0, 0], [1, 0.5]]}]})
    cue = {"version": 2, "id": "music/journey", "bus": "music", "priority": 6, "gain": 1,
           "tempo": {"numerator": 120, "denominator": 1}, "meter": {"beats": 4, "unit": 4},
           "initial_state": "exploring", "states": [{"id": "exploring", "section": "explore"},
                                                     {"id": "arrived", "section": "arrive"}],
           "sections": sections,
           "transitions": [{"from": "explore", "to": "arrive", "quantize": "beat", "fade_frames": 0}],
           "layer_ramp_frames": 240, "max_stingers": 1,
           "stingers": [{"id": "accent", "sound": "sound/accent", "quantize": "beat"}]}
    sound = {"version": 1, "id": "sound/accent", "bus": "music", "group": "accents", "priority": 6,
             "gain": 1, "rate": 1, "spatial": {"mode": "none", "min_distance": 1, "max_distance": 100},
             "loop": {"enabled": False, "begin_frame": 0, "end_frame": 0},
             "policy": {"cooldown_ms": 0, "suppress_while_active": False}, "variations": ["source/arrive"]}
    resources.append({"id": sound["id"], "kind": "sound", "path": "accent.json"})
    resources.append({"id": cue["id"], "kind": "music", "path": "journey.json"})
    for filename, value in (("accent.json", sound), ("journey.json", cue), ("catalog.json", {"version": 1, "resources": resources})):
        (root / filename).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
