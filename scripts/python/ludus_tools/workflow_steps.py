"""GitHub Actions steps shared by generated project workflows.

The release workflow (``release_setup``) and the build/test workflow
(``ci_setup``) provision the same pinned toolchain, host tools and Release SDK.
Each function returns literal YAML lines indented for a job's ``steps`` list or
a step's ``run`` block, so both workflows change together when a pin changes.
"""
from __future__ import annotations

from pathlib import Path

from .errors import CONFLICT, INVALID_PROJECT, ToolingError
from .toolchain import LLVM_APT_KEY_FINGERPRINT, PINNED_LLVM_MAJOR, pinned_llvm_tool

# The pinned Ludus host tools commit recorded by generated workflows.
TOOLS_REVISION_RELPATH = "config/ludus-tools-revision.txt"

CHECKOUT_STEP = '''      - uses: actions/checkout@93cb6efe18208431cddfb8368fd83d5badbf9bfd # v5.0.1 (Node 24)
        with:
          persist-credentials: false
'''

NATIVE_PLATFORMS = ("linux-x64", "macos-arm64", "macos-x64")

# Hosted runners cannot provision the pinned toolchain for these platforms.
# Homebrew ships no Intel macOS bottles for llvm, lld or python@3.12, and LLVM
# publishes no macOS x64 release binaries, so macos-15-intel would build LLVM
# from source for longer than a job may run.
_UNAVAILABLE_HOSTED_PLATFORMS = {
    "macos-x64": "macos-x64 workflows are unavailable: Homebrew ships no Intel macOS bottles for the pinned "
                 "LLVM and LLVM publishes no macOS x64 release binaries; choose linux-x64 or macos-arm64",
}
# The native platforms that generated workflows can build on hosted runners.
HOSTED_PLATFORMS = tuple(platform for platform in NATIVE_PLATFORMS if platform not in _UNAVAILABLE_HOSTED_PLATFORMS)

_LINUX_TOOLCHAIN = '''          # Ubuntu 24.04 does not package the pinned LLVM; trust only apt.llvm.org's published key.
          curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o "$RUNNER_TEMP/apt.llvm.org.asc"
          fingerprint="$(gpg --show-keys --with-colons "$RUNNER_TEMP/apt.llvm.org.asc" | awk -F: '/^fpr:/ { print $10; exit }')"
          [[ "$fingerprint" == LLVM_APT_KEY_TOKEN ]]
          sudo install -D -m 0644 "$RUNNER_TEMP/apt.llvm.org.asc" /etc/apt/keyrings/ludus-apt.llvm.org.asc
          printf 'Types: deb\\nURIs: https://apt.llvm.org/noble/\\nSuites: llvm-toolchain-noble-LLVM_MAJOR_TOKEN\\nComponents: main\\nSigned-By: /etc/apt/keyrings/ludus-apt.llvm.org.asc\\n' \\
            | sudo tee /etc/apt/sources.list.d/ludus-apt.llvm.org.sources > /dev/null
          sudo apt-get update -qq
          sudo apt-get install -y -qq python3-venv clang-LLVM_MAJOR_TOKEN lld-LLVM_MAJOR_TOKEN binutils libwayland-dev wayland-protocols
          python3 -m venv VENV_TOKEN
'''

_MACOS_TOOLCHAIN = '''          # An SDK accepts only the exact compiler version it was built with, and
          # runner images can carry stale formula metadata; install the current bottle.
          brew update --quiet
          brew install python@3.12 llvm@LLVM_MAJOR_TOKEN
          python_bin="$(brew --prefix python@3.12)/bin/python3.12"
          llvm_prefix="$(brew --prefix llvm@LLVM_MAJOR_TOKEN)"
          "$python_bin" -m venv VENV_TOKEN
          echo "$llvm_prefix/bin" >> "$GITHUB_PATH"
          echo 'MACOSX_DEPLOYMENT_TARGET=14.0' >> "$GITHUB_ENV"
          echo "CXXFLAGS=-stdlib=libc++ -nostdinc++ -isystem $llvm_prefix/include/c++/v1" >> "$GITHUB_ENV"
'''

_HOST_TOOLS = '''          ref=$(cat TOOLS_REVISION_TOKEN)
          [[ "$ref" =~ ^[0-9a-f]{40}$ ]]
          VENV_TOKEN/bin/pip install cmake==3.29.6 ninja==1.11.1.3
          VENV_TOKEN/bin/pip install "ludus-tools @ git+https://github.com/The-Ludus-Project/Ludus.git@$ref#subdirectory=scripts/python"
          echo VENV_TOKEN/bin >> "$GITHUB_PATH"
'''

_SDK_STEP = r"""      - name: STEP_NAME_TOKEN
        env:
          SDK_URL: ${{ vars.URL_VARIABLE_TOKEN }}
          SDK_SHA256: ${{ vars.SHA_VARIABLE_TOKEN }}
        run: |
          python3 - <<'PYTHON'
          import hashlib, json, os, re, subprocess, urllib.request
          url, expected = os.environ['SDK_URL'], os.environ['SDK_SHA256']
          if not url.startswith('https://') or not re.fullmatch('[a-f0-9]{64}', expected):
              raise SystemExit('INVALID_SDK_TOKEN')
          digest, total = hashlib.sha256(), 0
          path = os.environ['RUNNER_TEMP'] + '/ludus-release-sdk.tar'
          with urllib.request.urlopen(url, timeout=30) as source, open(path, 'wb') as output:
              if not source.geturl().startswith('https://'):
                  raise SystemExit('SDK redirect outside HTTPS')
              while block := source.read(1024 * 1024):
                  total += len(block)
                  if total > 4 * 1024 * 1024 * 1024:
                      raise SystemExit('SDK download exceeds limit')
                  digest.update(block)
                  output.write(block)
          if digest.hexdigest() != expected:
              raise SystemExit('SDK archive digest mismatch')
          # Leave stderr on the job log so an install failure shows its diagnostic.
          result = subprocess.run(['ludus', '--json', 'sdk', 'install', '--archive', path, '--digest', expected], text=True, stdout=subprocess.PIPE, check=True)
          prefix = json.loads(result.stdout)['installed']['prefix']
          with open(os.environ['GITHUB_ENV'], 'a') as output:
              output.write('LUDUS_RELEASE_SDK_PREFIX=' + prefix + '\n')
          PYTHON
"""


def has_tools_revision(project_dir: Path, tools_ref: str) -> bool:
    """Return whether the project already pins ``tools_ref``; fail with ``Conflict`` if it pins another commit.

    Release and CI setup share the pin, so whichever runs second reuses it.
    """
    path = project_dir / TOOLS_REVISION_RELPATH
    if path.is_symlink() or path.exists() is False:
        return False
    if path.is_file() is False or path.read_text(encoding="utf-8").strip() != tools_ref:
        raise ToolingError(CONFLICT, f"{TOOLS_REVISION_RELPATH} names a different Ludus commit; "
                                     f"pass that --tools-ref or update the file yourself")
    return True


def require_hosted_toolchain(platform: str) -> None:
    """Fail with ``InvalidProject`` when a generated workflow cannot install the pinned toolchain for ``platform``."""
    reason = _UNAVAILABLE_HOSTED_PLATFORMS.get(platform)
    if reason is not None:
        raise ToolingError(INVALID_PROJECT, reason)


def runner_for(platform: str) -> str:
    """Return the GitHub-hosted runner label for a native platform."""
    # GitHub, GitHub-hosted runners reference: explicit native CPU labels.
    # https://docs.github.com/en/actions/reference/runners/github-hosted-runners
    # macOS 14 images are deprecated (actions/runner-images#13518); SDKs keep a 14.0 deployment floor.
    # https://github.com/actions/runner-images/issues/13518
    return {"linux-x64": "ubuntu-24.04", "macos-arm64": "macos-15", "macos-x64": "macos-15-intel"}[platform]


def pinned_compiler() -> str:
    """Return the versioned C++ compiler that ``build_prerequisites`` installs on Linux."""
    return pinned_llvm_tool("clang++")


def _pin(text: str, venv: str) -> str:
    return (text.replace("LLVM_MAJOR_TOKEN", str(PINNED_LLVM_MAJOR))
            .replace("LLVM_APT_KEY_TOKEN", LLVM_APT_KEY_FINGERPRINT)
            .replace("TOOLS_REVISION_TOKEN", TOOLS_REVISION_RELPATH)
            .replace("VENV_TOKEN", venv))


def build_prerequisites(platform: str, venv: str) -> str:
    """Return ``run`` lines installing the pinned compiler and host tools into ``venv``."""
    toolchain = _MACOS_TOOLCHAIN if platform.startswith("macos-") else _LINUX_TOOLCHAIN
    return _pin(toolchain + _HOST_TOOLS, venv)


def host_tools(venv: str) -> str:
    """Return ``run`` lines installing only the pinned host tools (no compiler) into ``venv``."""
    return _pin("          python3 -m venv VENV_TOKEN\n" + _HOST_TOOLS, venv)


def release_sdk_step(name: str, invalid_message: str, *, url_variable: str = "LUDUS_RELEASE_SDK_URL",
                     sha_variable: str = "LUDUS_RELEASE_SDK_SHA256") -> str:
    """Return a step that installs the digest-pinned Release SDK named by repository variables.

    The step exports ``LUDUS_RELEASE_SDK_PREFIX`` and fails the job with
    ``invalid_message`` unless ``url_variable`` holds an HTTPS URL and
    ``sha_variable`` a SHA-256 digest.
    """
    return (_SDK_STEP.replace("STEP_NAME_TOKEN", name).replace("INVALID_SDK_TOKEN", invalid_message)
            .replace("URL_VARIABLE_TOKEN", url_variable).replace("SHA_VARIABLE_TOKEN", sha_variable))
