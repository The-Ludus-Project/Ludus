"""Run the installed consumer and independently locate its marker in exported PCM."""
import array
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

from generate_transition_fixture import MARKER_OFFSET, RATE


def verify(executable, content, output):
    with tempfile.TemporaryDirectory(dir=output.parent) as directory:
        raw = Path(directory) / "replacement.f32"
        subprocess.run([executable, content, raw], check=True)
        samples = array.array("f")
        samples.frombytes(raw.read_bytes())
    pcm = array.array("h", (round(max(-1.0, min(1.0, value)) * 32767) for value in samples))
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(output), "wb") as rendered:
        rendered.setparams((2, 2, RATE, len(samples) // 2, "NONE", "not compressed"))
        rendered.writeframes(pcm.tobytes())

    with wave.open(str(output), "rb") as rendered:
        pcm = array.array("h", rendered.readframes(rendered.getnframes()))
    if sys.byteorder != "little":
        pcm.byteswap()
    markers = [frame for frame, value in enumerate(pcm[1::2]) if value > 3276]
    if markers != [RATE + MARKER_OFFSET] or max(pcm[:RATE * 2:2]) < 3276:
        raise ValueError(f"Unexpected outgoing audio or marker frames {markers}")
    print(f"Rendered WAV marker frame {markers[0]} in {output}")


if __name__ == "__main__":
    verify(sys.argv[1], sys.argv[2], Path(sys.argv[3]))
