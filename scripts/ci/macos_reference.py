"""Commands of the macOS GPU reference oracle batch job (ci.yml, macos-reference).

The CPU/GPU reference transport cases (ludus_renderer_reference: ...) took 30 of the
macOS native job's 89 minutes in batch 44859003421, one at a time on one GPU. This job
runs them beside the native job: it configures the same tree, restores the same
compiler cache, and builds only their test executable.
scripts/python/ci_checks.py runs these checks; see
docs/development/continuous-integration.md#batch-job-check-plans.
"""
from ci_checks import Check

CHECKS = [
    Check('build', 'Build the renderer reference test executable',
          env={'CONAN_HOME': '$GITHUB_WORKSPACE/out/conan/home', 'CONAN_NON_INTERACTIVE': '1'},
          run=r"""export PATH="$PWD/out/host-tools/bin:$PWD/out/host-tools/venv/bin:$PATH"
out/host-tools/venv/bin/cmake -S . -B out/build/macos-clang-development \
  -DLUDUS_BUILD_BEHAVIOR=ON -DLUDUS_BUILD_BEHAVIOR_ACCEPTANCE=ON -DLUDUS_WARNINGS_AS_ERRORS=ON \
  -DLUDUS_BUILD_WORLD_DEMO=ON -DLUDUS_BUILD_SHADER_WORKER=ON -DLUDUS_EDITOR_CRASH_CAPTURE_TESTS=ON
out/host-tools/venv/bin/cmake --build --preset macos-clang-development --target ludus_renderer_render_tests
"""),
    Check('reference-tests', 'Test CPU and GPU reference transport on Metal', needs=('build',),
          run=r"""out/host-tools/venv/bin/ctest --preset macos-clang-development --tests-regex '^ludus_renderer_reference: ' --no-tests=error --output-junit "$RUNNER_TEMP/macos-reference-tests.xml"
"""),
    Check('timeout-headroom', 'Report CTest timeout headroom', needs=('build',),
          run=r"""python3 scripts/python/ctest_headroom.py out/build/macos-clang-development --ctest out/host-tools/venv/bin/ctest --junit "$RUNNER_TEMP/macos-reference-tests.xml" --title "macOS GPU reference tests: CTest timeout headroom"
"""),
]
