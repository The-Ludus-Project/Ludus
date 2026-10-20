"""Exercise real wasm/C API callbacks using an explicit test WebGPU provider."""
from pathlib import Path
import subprocess
import sys
node, executable = sys.argv[1:]
for scenario in ("success", "surface-failure", "shader-failure", "acquisition-failure",
                 "cancel-pipeline", "offscreen", "resize-hidden", "device-loss", "validation"):
    result = subprocess.run([node, executable, scenario], cwd=Path(executable).parent,
                            capture_output=True, text=True, timeout=10)
    output = result.stdout + result.stderr
    assert result.returncode == 0 and "[W5:passed]" in output, scenario + "\n" + output
    print(output.strip())
