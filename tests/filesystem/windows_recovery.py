"""Exercise process interruption around Windows publication on the runner's filesystem."""

from pathlib import Path
import subprocess
import sys
import tempfile


INTERRUPTED = 78


def verify(probe: Path) -> None:
    for was_present in (False, True):
        for phase in ("before", "after"):
            with tempfile.TemporaryDirectory(prefix="ludus 音 recovery ") as directory:
                root = Path(directory)
                output = root / "output.bin"
                if was_present:
                    output.write_bytes(b"old")
                run = subprocess.run([probe, phase], cwd=root, check=False, timeout=10)
                if run.returncode != INTERRUPTED:
                    raise RuntimeError(f"{phase}: expected abrupt termination, got {run.returncode}")
                actual = output.read_bytes() if output.exists() else None
                expected = b"new!" if phase == "after" else (b"old" if was_present else None)
                if actual != expected:
                    raise RuntimeError(f"{phase}: expected {expected!r}, got {actual!r}")
                # Recovery must tolerate a retained temporary and release the
                # dead writer's cooperative lock without operator intervention.
                subprocess.run([probe, "retry"], cwd=root, check=True, timeout=10)
                if output.read_bytes() != b"new!":
                    raise RuntimeError(f"{phase}: retry did not publish the complete bytes")


def verify_junction(probe: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="ludus junction ") as directory:
        root = Path(directory)
        target = root / "target"
        target.mkdir()
        output = target / "output.bin"
        output.write_bytes(b"old")
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", "link", "target"],
                       cwd=root, check=True, timeout=10)
        subprocess.run([probe, "reject-junction"], cwd=root, check=True, timeout=10)
        if output.read_bytes() != b"old":
            raise RuntimeError("Publication crossed the child junction")


if __name__ == "__main__":
    probe = Path(sys.argv[1]).resolve()
    verify(probe)
    verify_junction(probe)
