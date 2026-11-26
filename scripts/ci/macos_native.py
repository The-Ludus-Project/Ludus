"""Commands of the macOS native backends and diagnostics batch job (ci.yml, macos-compile).

scripts/python/ci_checks.py runs these checks in order, skips a check whose needs did not
succeed, continues past failures and reports them all. Keep commands here rather than in
workflow YAML: batches run main's workflow definitions but the candidate's plan.
See docs/development/continuous-integration.md#batch-job-check-plans.
"""
from ci_checks import Check

CHECKS = [
    Check('build', 'Build all enabled engine and application targets', env={'CONAN_HOME': '$GITHUB_WORKSPACE/out/conan/home', 'CONAN_NON_INTERACTIVE': '1'},
          run=r"""export PATH="$PWD/out/host-tools/bin:$PWD/out/host-tools/venv/bin:$PATH"
out/host-tools/venv/bin/cmake -S . -B out/build/macos-clang-development \
  -DLUDUS_BUILD_BEHAVIOR=ON -DLUDUS_BUILD_BEHAVIOR_ACCEPTANCE=ON -DLUDUS_WARNINGS_AS_ERRORS=ON \
  -DLUDUS_BUILD_WORLD_DEMO=ON -DLUDUS_BUILD_SHADER_WORKER=ON -DLUDUS_EDITOR_CRASH_CAPTURE_TESTS=ON
out/host-tools/venv/bin/cmake --build --preset macos-clang-development
"""),
    # Static analysis needs only the compile database and generated headers. Run it before the
    # long test checks so an analyzer finding is reported in minutes.
    Check('cocoa-analysis', 'Analyze Cocoa Objective-C++ production and test sources', needs=('build',),
          run=r"""out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/platform/src/window_cocoa.mm
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/platform/tests/cocoa_tests.mm
"""),
    Check('metal-analysis', 'Analyze Metal production and rendering tests', needs=('build',),
          run=r"""out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/graphics/rhi/src/rhi_metal.mm
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/graphics/rhi/tests/metal_tests.mm
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/graphics/rhi/tests/render_tests.cpp
"""),
    Check('editor-analysis', 'Analyze native Editor production and regression sources', needs=('build',),
          run=r"""./scripts/check macos-clang-development --tidy-sources \
  apps/editor/src/controller_play.cpp apps/editor/src/workspace.cpp \
  apps/editor/src/main_window.cpp apps/editor/src/main_window_layout.cpp \
  apps/editor/src/recent_projects.cpp apps/editor/tests/controller_tests.cpp \
  apps/editor/src/world_demo_adapter.cpp \
  apps/editor/src/world_document.cpp apps/editor/src/world_workspace.cpp \
  apps/editor/src/render_scene.cpp apps/editor/src/render_preview.cpp \
  apps/editor/src/editor_camera_controller.cpp apps/editor/src/viewport_navigation.cpp \
  apps/editor/tests/viewport_navigation_tests.cpp \
  apps/editor/src/preview_viewport.cpp apps/editor/src/qt_viewport_device_metal.mm \
  apps/editor/src/level_workspace.cpp apps/editor/tests/level_workspace_tests.cpp \
  apps/editor/src/render_load_job.cpp apps/editor/src/render_panel.cpp \
  apps/editor/tests/render_authoring_tests.cpp \
  apps/editor/tests/world_workspace_tests.cpp \
  apps/editor/tests/world_demo_workspace_tests.cpp apps/editor/tests/render_native_tests.mm \
  apps/editor/tests/macos_journey_tests.cpp apps/editor/tests/live_journey_tests.cpp
"""),
    Check('filesystem-analysis', 'Analyze native filesystem production and test sources', needs=('build',),
          run=r"""out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/src/posix.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/file_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/posix_fault_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/src/namespace.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/namespace_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/namespace_fault_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/src/async.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/src/async_posix.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/async_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/async_fault_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/src/persistence_posix.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/src/watch.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' --extra-arg=-DLUDUS_FILESYSTEM_PERSISTENCE_FAULT_TESTING=1 modules/foundation/filesystem/src/persistence_posix.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' --extra-arg=-DLUDUS_FILESYSTEM_WATCH_FAULT_TESTING=1 modules/foundation/filesystem/src/watch.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/persistence_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/watch_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/persistence_fault_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/watch_fault_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/src/pack.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/src/pack_codec.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/pack_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/filesystem/tests/pack_fault_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/content/tests/native_reads_tests.cpp
"""),
    Check('content-analysis', 'Analyze Content native persistence and tests', needs=('build',),
          run=r"""out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/content/src/files.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' --extra-arg=-DLUDUS_CONTENT_SAVE_FAULT_TESTING=1 modules/content/src/files.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/content/tests/content_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/content/tests/native_saves_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/content/tests/save_fault_tests.cpp
"""),
    Check('audio-analysis', 'Analyze native Audio workers, device adapter and regressions', needs=('build',),
          run=r"""out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/audio/src/device.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/audio/tests/device_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/audio/tests/stream_tests.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/audio/content/src/loader.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/audio/content/tests/content_tests.cpp
"""),
    Check('behavior-provider', 'Qualify production Behavior provider on macOS', needs=('build',),
          run=r"""out/host-tools/venv/bin/ctest --preset macos-clang-development --tests-regex '^ludus_behavior_' --output-on-failure --no-tests=error
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/runtime/scripting/src/runtime.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/runtime/behavior/src/behavior.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' tools/script-provider/safety.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' tools/script-provider/fault_allocator.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' tools/script-provider/allocator_contract.cpp
"""),
    # Load-independent: each source compiles in one target (docs/development/build-profiling.md).
    Check('build-graph', 'Check that each source compiles once', needs=('build',),
          run=r"""out/host-tools/venv/bin/python tools/check_build_graph.py out/build/macos-clang-development --summary "$GITHUB_STEP_SUMMARY"
"""),
    Check('source-inventory', 'Require complete macOS source inventory', needs=('build',),
          run=r"""./scripts/check macos-clang-development --inventory
"""),
    Check('setup-contracts', 'Test macOS setup contracts', needs=('build',),
          run=r"""PYTHONPATH=scripts/python python3 -m unittest test_macos_setup test_init_launcher test_init_ui test_rhi_measurements test_metal_shaders test_raster_shaders test_texture_cooker test_macos_editor test_editor_tool test_process_ownership test_play_probe test_play_session test_play_build test_play_documents test_play_assets -v
"""),
    # The CPU/GPU reference oracle cases run in the macos-reference job (scripts/ci/macos_reference.py).
    Check('native-tests', 'Test native Cocoa, Metal, filesystem, threading, Content and Audio', needs=('build',), env={'LUDUS_TEST_COCOA': '1'},
          run=r"""out/host-tools/venv/bin/ctest --preset macos-clang-development --label-regex 'platform|macos|filesystem|threading|content|audio|build-contract|diagnostic-startup|editor|loader|session|reload|protocol|acceptance|shader' --exclude-regex '^ludus_renderer_reference: ' --no-tests=error --output-junit "$RUNNER_TEMP/macos-native-tests.xml"
"""),
    Check('timeout-headroom', 'Report CTest timeout headroom', needs=('build',),
          run=r"""python3 scripts/python/ctest_headroom.py out/build/macos-clang-development --ctest out/host-tools/venv/bin/ctest --junit "$RUNNER_TEMP/macos-native-tests.xml" --title "macOS native tests: CTest timeout headroom"
"""),
    Check('debug-helper', 'Test native Debug helper and explicit assertion decisions', needs=('build',),
          run=r"""./init.sh --cli --preset macos-clang-debug --preset-only --locked --with-tests --no-system-install
# Only these controlled test children enable dialog capability. Their
# drivers clear CI and own a PTY; production builds remain noninteractive.
out/host-tools/venv/bin/cmake --preset macos-clang-debug -DLUDUS_CI_BUILD=OFF -DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc"
out/host-tools/venv/bin/cmake --build --preset macos-clang-debug --target ludus_diagnostic_startup_child ludus_assert_decision_child
out/host-tools/venv/bin/ctest --preset macos-clang-debug --label-regex diagnostic-startup --no-tests=error
"""),
    Check('diagnostic-analysis', 'Analyze native diagnostic transport and startup sources', needs=('build', 'debug-helper'),
          run=r"""out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/base/src/diagnostic_output.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' tools/diagnostics/src/session.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-debug --warnings-as-errors='*' tools/diagnostics/tests/diagnostic_startup_child.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-debug --warnings-as-errors='*' tools/diagnostics/tests/assert_decision_child.cpp
"""),
    # Installed-SDK checks need only this install, so a failing helper check does not skip them.
    Check('sdk-install', 'Install the macOS development SDK', needs=('build',),
          run=r"""out/host-tools/venv/bin/cmake --install out/build/macos-clang-development
"""),
    Check('metal-shader-helper', 'Verify installed Metal shader helper', needs=('sdk-install',),
          run=r"""out/host-tools/venv/bin/python tests/sdk_consumer/verify_metal_shader_build.py --sdk "$PWD/out/install/macos-clang-development" --cmake "$PWD/out/host-tools/venv/bin/cmake" --ninja "$PWD/out/host-tools/venv/bin/ninja" --cxx "$PWD/out/host-tools/bin/clang++" --slang "$PWD/out/shader-tools/slang/bin/slangc" --conan "$PWD/out/conan/macos-clang-development" --sysroot "$PWD/out/host-tools/macos-sdk" --libcxx "$PWD/out/host-tools/libcxx-include"
"""),
    Check('renderer-consumers', 'Verify installed L0/L1/L2/L3/L4/L5 renderer consumers', needs=('sdk-install',),
          run=r"""out/host-tools/venv/bin/cmake -S tests/sdk_consumer/renderer -B out/renderer-sdk -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++" -DCMAKE_MAKE_PROGRAM="$PWD/out/host-tools/venv/bin/ninja" \
  -DCMAKE_PREFIX_PATH="$PWD/out/install/macos-clang-development;$PWD/out/conan/macos-clang-development" \
  -DCMAKE_OSX_SYSROOT="$PWD/out/host-tools/macos-sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++ -nostdinc++ -isystem $PWD/out/host-tools/libcxx-include" \
  -DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc" \
  -DLUDUS_SPIRV_CROSS="$PWD/out/shader-tools/spirv-cross/bin/spirv-cross"
out/host-tools/venv/bin/cmake --build out/renderer-sdk --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
out/renderer-sdk/ludus_sdk_renderer_consumer
out/host-tools/venv/bin/ctest --test-dir out/renderer-sdk -R "^installed_encoder$" --no-tests=error --output-on-failure
out/renderer-sdk/ludus_sdk_renderer_views_consumer
out/renderer-sdk/ludus_sdk_renderer_overlays_consumer
out/renderer-sdk/ludus_sdk_ui_render_consumer
out/renderer-sdk/ludus_sdk_ui_text_consumer
out/renderer-sdk/ludus_sdk_renderer_materials_consumer
out/renderer-sdk/ludus_sdk_renderer_lighting_consumer
out/renderer-sdk/ludus_sdk_renderer_scaling_consumer
out/host-tools/venv/bin/ctest --test-dir out/renderer-sdk/environment --output-on-failure --no-tests=error -R '^ludus_sdk_environment$'
out/host-tools/venv/bin/ctest --test-dir out/renderer-sdk --output-on-failure --no-tests=error -R "^ludus_sdk_hybrid_indirect_diffuse_consumer$"
out/host-tools/venv/bin/ctest --test-dir out/renderer-sdk -V --no-tests=error -R "^ludus_sdk_ray_storage_"
out/host-tools/venv/bin/ctest --test-dir out/renderer-sdk --output-on-failure --no-tests=error -R "^ludus_sdk_patch_seam_(geometry|raster)$"
out/renderer-sdk/ludus_sdk_render_graph_runtime_consumer
out/renderer-sdk/ludus_sdk_render_graph_bilinear_consumer 2>&1 | tee out/bilinear-sdk-metal.log
python3 -c 'from pathlib import Path; text=Path("out/bilinear-sdk-metal.log").read_text(); assert "bilinear passed" in text, text'
mkdir -p out/rg4-evidence
out/renderer-sdk/ludus_sdk_render_graph_pooling_consumer 2>&1 | tee out/rg4-evidence/metal.log
python3 -c 'from pathlib import Path; text=Path("out/rg4-evidence/metal.log").read_text(); assert "pooling passed" in text, text'
out/host-tools/venv/bin/cmake --install out/renderer-sdk --component AuthoredPooling --prefix "$PWD/out/packaged-consumer"
set -o pipefail
out/packaged-consumer/bin/ludus_sdk_authored_pooling 2>&1 | tee out/rg4-evidence/authored-metal.log
out/renderer-sdk/ludus_sdk_reference_capture out/reference.luhdr
python3 scripts/reference-hdr out/reference.luhdr --pfm out/reference.pfm --manifest out/reference.json
out/host-tools/venv/bin/ctest --test-dir out/renderer-sdk/reference-scene --output-on-failure
out/host-tools/bin/clang-tidy -p out/renderer-sdk --warnings-as-errors='*' examples/reference-scene/src/main.cpp
out/host-tools/venv/bin/python - <<'PYFORMAT'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/python')
from formatting import format_source
path = Path('examples/reference-scene/src/main.cpp')
assert format_source(path.read_text(), path, 'out/host-tools/bin/clang-format') == path.read_text()
PYFORMAT
"""),
    Check('mesh-light-seams', 'Measure mesh-light cube seams on Metal', needs=('renderer-consumers',),
          run=r"""out/host-tools/venv/bin/python -m venv out/shadow-analysis
out/shadow-analysis/bin/python -m pip install numpy==2.5.5
build=out/build/macos-clang-development
graph="$build/modules/graphics/renderer/ludus-graphs/ludus_renderer_render_tests/rg2_clustered_Default/rg2_clustered_Default.lurgrt"
out/shadow-analysis/bin/python tests/sdk_consumer/mesh_shadow/qualify.py out/shadow-worlds "$graph" \
  --editor "$build/apps/editor/ludus_editor_tests" --sdk out/renderer-sdk/mesh-shadow/ludus_sdk_mesh_shadow
"""),
    Check('game-project-tooling', 'Verify installed macOS game-project tooling', needs=('sdk-install',),
          run=r"""./scripts/install-sdk macos-clang-development
out/host-tools/venv/bin/python -m venv out/project-tooling-venv
out/project-tooling-venv/bin/python -m pip install --no-deps ./scripts/python
PATH="$PWD/out/host-tools/venv/bin:$PWD/out/host-tools/bin:$PATH" PYTHONPATH=scripts/python out/host-tools/venv/bin/python -m unittest test_project_setup test_engine_pin test_macos_project_tooling test_creation_engine test_ludus_tools test_ludus_project_ops test_ludus_cli test_render_graph test_render_graph_build test_render_graph_execution test_render_graph_authoring test_render_graph_buffers -v
out/host-tools/venv/bin/python tests/sdk_consumer/verify_macos_project_tooling.py --sdk "$PWD/out/install/macos-clang-development" --tools "$PWD" --cli "$PWD/out/project-tooling-venv/bin/ludus"
"""),
    Check('rg1-packages', 'Qualify installed RG1 Metal and WGSL packages', needs=('sdk-install',),
          run=r"""out/host-tools/venv/bin/python tests/sdk_consumer/verify_render_graph_build.py \
  --cli "$PWD/out/project-tooling-venv/bin/ludus" --sdk "$PWD/out/install/macos-clang-development" \
  --slang "$PWD/out/shader-tools/slang/bin/slangc" --cxx "$PWD/out/host-tools/bin/clang++" \
  --profiles metal wgsl --sanitize --output out/rg1-macos
out/host-tools/venv/bin/python tests/sdk_consumer/verify_render_graph_execution.py \
  --cli "$PWD/out/project-tooling-venv/bin/ludus" --sdk "$PWD/out/install/macos-clang-development" \
  --slang "$PWD/out/shader-tools/slang/bin/slangc" --cxx "$PWD/out/host-tools/bin/clang++" \
  --cmake "$PWD/out/host-tools/venv/bin/cmake" --profiles metal wgsl --output out/rg2-macos
"""),
    # Links the engine ray kernels (software and native) against the SDK's staged helper files,
    # so an engine include that the helper does not stage fails here, not in user projects.
    Check('render-graph-fixtures', 'Compile render-graph ray, compute and build fixtures with the installed helper', needs=('sdk-install',), env={'LUDUS_RG1_SLANG': '$GITHUB_WORKSPACE/out/shader-tools/slang/bin/slangc', 'LUDUS_RG1_CROSS': '$GITHUB_WORKSPACE/out/shader-tools/spirv-cross/bin/spirv-cross', 'LUDUS_RG1_HELPER': '$GITHUB_WORKSPACE/out/install/macos-clang-development/lib/cmake/Ludus/shaders'},
          run=r"""PYTHONPATH=scripts/python out/host-tools/venv/bin/python -m unittest test_render_graph_ray test_render_graph_compute test_render_graph_build -v
"""),
    Check('release-packages', 'Verify signed macOS game release packages', needs=('sdk-install',), env={'QT_QPA_PLATFORM': 'cocoa'}, timeout_minutes=30,
          run=r"""./init.sh --cli --preset macos-clang-release --preset-only --locked --no-system-install
out/host-tools/venv/bin/cmake --preset macos-clang-release -DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc"
./scripts/install-sdk macos-clang-release
PATH="$PWD/out/host-tools/venv/bin:$PWD/out/host-tools/bin:$PATH" PYTHONPATH=scripts/python out/host-tools/venv/bin/python -m unittest test_macos_release test_ludus_release test_ludus_itch_release -v
out/host-tools/venv/bin/python tests/sdk_consumer/verify_macos_release.py --sdk "$PWD/out/install/macos-clang-release" --tools "$PWD" --cli "$PWD/out/project-tooling-venv/bin/ludus"
LUDUS_SETUP_TEST_SDK="$PWD/out/install/macos-clang-development" LUDUS_RELEASE_TEST_SDK="$PWD/out/install/macos-clang-release" out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.macos-release-journey]'
"""),
    Check('lighting-room', 'Qualify Lighting Room in hosted Play and relocated Release', needs=('sdk-install',), env={'QT_QPA_PLATFORM': 'cocoa'}, timeout_minutes=10,
          run=r"""export PATH="$PWD/out/host-tools/venv/bin:$PWD/out/host-tools/bin:$PATH"
sdk="$PWD/out/install/macos-clang-release"
sample=examples/lighting-room
./scripts/ludus project repair "$sample" --tools "$PWD" --sdk "$sdk" --profile macos-clang-release --no-web
# The JSON result names the failure; show it rather than only the exit status.
./scripts/ludus --json project package "$sample" --profile macos-release --version 0.1.0 --sdk "$sdk" > "$RUNNER_TEMP/lighting-room-package.log" ||
  { cat "$RUNNER_TEMP/lighting-room-package.log"; exit 1; }
package_dir=$(tail -n 1 "$RUNNER_TEMP/lighting-room-package.log" | jq -er '.package')
./scripts/ludus project package verify "$package_dir"
relocated=$(mktemp -d "$RUNNER_TEMP/Lighting Room.XXXXXX")
ditto -x -k "$package_dir/game.zip" "$relocated"
export LUDUS_LIGHTING_ROOM_PLAYER="$relocated/lighting_room.app/Contents/MacOS/lighting_room"
python "$sample/qualify.py" "$LUDUS_LIGHTING_ROOM_PLAYER"
python "$sample/qualify.py" "$LUDUS_LIGHTING_ROOM_PLAYER"
out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.lighting-room-hosted]'
"""),
    # LUSL L5d wave 2 (issue 1736): the renderer_composite recipe fixture, built against a relocated
    # copy of the installed SDK and bound by the recipe's generated slots. The renderer GPU suite
    # prints the same COMPOSITE_RECIPE_FIXTURE hash for its source-tree run.
    Check('composite-recipe-sdk', 'Draw the composite recipe fixture from a relocated SDK', needs=('sdk-install',),
          run=r"""relocated=$(mktemp -d "$RUNNER_TEMP/Ludus SDK.XXXXXX")
ditto out/install/macos-clang-development "$relocated/sdk"
out/host-tools/venv/bin/cmake -S tests/sdk_consumer/composite_recipe -B out/composite-recipe-sdk -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++" -DCMAKE_MAKE_PROGRAM="$PWD/out/host-tools/venv/bin/ninja" \
  -DCMAKE_PREFIX_PATH="$relocated/sdk;$PWD/out/conan/macos-clang-development" \
  -DCMAKE_OSX_SYSROOT="$PWD/out/host-tools/macos-sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++ -nostdinc++ -isystem $PWD/out/host-tools/libcxx-include" \
  -DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc" \
  -DLUDUS_SPIRV_CROSS="$PWD/out/shader-tools/spirv-cross/bin/spirv-cross"
out/host-tools/venv/bin/cmake --build out/composite-recipe-sdk
out/host-tools/venv/bin/ctest --test-dir out/composite-recipe-sdk -V --no-tests=error -R "^ludus_sdk_composite_recipe$"
"""),
    Check('behavior-sdk', 'Qualify relocated Behavior SDK and extracted macOS player', needs=('sdk-install',),
          run=r"""out/host-tools/venv/bin/python tools/script-provider/verify_consumer.py out/install/macos-clang-development
"""),
    Check('editor-lifecycle', 'Verify native Cocoa Editor project lifecycle', needs=('sdk-install',), env={'QT_QPA_PLATFORM': 'cocoa'}, timeout_minutes=15,
          run=r"""LUDUS_SETUP_TEST_SDK="$PWD/out/install/macos-clang-development" out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.macos-journey]'
"""),
    Check('editor-live-play', 'Verify native Cocoa Editor live play and hot reload', needs=('sdk-install',), env={'QT_QPA_PLATFORM': 'cocoa'}, timeout_minutes=15,
          run=r"""LUDUS_SDK_PREFIX="$PWD/out/install/macos-clang-development" out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.live-journey]'
"""),
    Check('hdr-import', 'Verify HDR companion import and Lit World highlights against the installed SDK', needs=('renderer-consumers',),
          run=r"""out/host-tools/venv/bin/python tests/sdk_consumer/environment/qualify.py \
  out/renderer-sdk/environment/ludus_sdk_environment --case world \
  --editor out/build/macos-clang-development/apps/editor/ludus_editor_tests
"""),
    Check('world-renderer', 'Verify native World renderer and camera authoring', needs=('build',), env={'QT_QPA_PLATFORM': 'cocoa'}, timeout_minutes=15,
          run=r"""out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.render-native],[.level-native]'
out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.authored-pooling-viewport]'
"""),
    Check('play-viewport', 'Verify Play in the World viewport', needs=('build',), env={'QT_QPA_PLATFORM': 'cocoa'}, timeout_minutes=15,
          run=r"""out/build/macos-clang-development/apps/editor/ludus_editor_tests '[.play-native]'
"""),
    Check('diagnostic-helper', 'Verify installed macOS diagnostic helper', needs=('sdk-install',),
          run=r"""out/host-tools/venv/bin/python tools/diagnostics/tests/startup_tests.py "$PWD/out/install/macos-clang-development/bin/ludus_diagnostic_helper" "$PWD/out/build/macos-clang-development/tools/diagnostics/ludus_diagnostic_startup_child" 0
"""),
    Check('content-persistence', 'Verify installed Content persistence', needs=('sdk-install',),
          run=r"""out/host-tools/venv/bin/cmake -S tests/sdk_consumer/content -B out/build/content-consumer -G Ninja \
  -DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++" \
  -DCMAKE_MAKE_PROGRAM="$PWD/out/host-tools/venv/bin/ninja" \
  -DCMAKE_OSX_SYSROOT="$PWD/out/host-tools/macos-sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++ -nostdinc++ -isystem \"$PWD/out/host-tools/libcxx-include\"" \
  -DCMAKE_PREFIX_PATH="$PWD/out/install/macos-clang-development;$PWD/out/conan/macos-clang-development" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
out/host-tools/venv/bin/cmake --build out/build/content-consumer --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
out/build/content-consumer/ludus_content_consumer
out/host-tools/bin/clang-tidy -p out/build/content-consumer --warnings-as-errors='*' tests/sdk_consumer/content.cpp
"""),
    Check('cornell-box', 'Build and test the independent Cornell box tutorial (Metal)', needs=('sdk-install',),
          run=r"""export PATH="$PWD/out/host-tools/bin:$PWD/out/host-tools/venv/bin:$PATH"
export LUDUS_SDK_PREFIX="$PWD/out/install/macos-clang-development;$PWD/out/conan/macos-clang-development"
export LUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc"
export LUDUS_OSX_SYSROOT="$PWD/out/host-tools/macos-sdk"
export LUDUS_LIBCXX_INCLUDE="$PWD/out/host-tools/libcxx-include"
cd examples/cornell-box
cmake --list-presets=configure
cmake --list-presets=build
cmake --list-presets=test
cmake --preset macos-clang-development
cmake --build --preset macos-clang-development
ctest --preset macos-clang-development
for source in src/main.cpp src/camera.cpp tests/camera_tests.cpp; do
  clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' "$source"
done
cmake --preset macos-clang-development -B out/sanitized -DCORNELL_ENABLE_SANITIZERS=ON
cmake --build out/sanitized
ctest --test-dir out/sanitized --output-on-failure --no-tests=error
out/build/macos-clang-development/cornell_box.app/Contents/MacOS/cornell_box --frames=3
"""),
    Check('world-demo', 'Build and run world demo with owned number parsing (Metal)', needs=('build',), timeout_minutes=8,
          run=r"""out/host-tools/venv/bin/cmake --preset macos-clang-development -DLUDUS_BUILD_WORLD_DEMO=ON -DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc"
out/host-tools/venv/bin/cmake --build --preset macos-clang-development --target ludus_world_demo ludus_world_demo_tests ludus_world_demo_allocation_tests ludus_world_render_items_tests ludus_world_frame_renderer_tests ludus_foundation_base_tests
out/host-tools/venv/bin/ctest --preset macos-clang-development --tests-regex '^(ludus_foundation_base_tests|ludus_world_demo_tests|ludus_world_demo_allocation_tests|ludus_world_render_items_tests|ludus_world_frame_renderer_pixels|ludus_world_shader_layout)$' --no-tests=error
out/build/macos-clang-development/apps/world_demo/ludus_world_demo --headless
out/build/macos-clang-development/apps/world_demo/ludus_world_demo --frames 3
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/base/src/parse_number.cpp
out/host-tools/bin/clang-tidy -p out/build/macos-clang-development --warnings-as-errors='*' modules/foundation/base/tests/parse_number_tests.cpp
"""),
    Check('audio-sdk', 'Validate installed Audio SDK and native workers', needs=('sdk-install',),
          run=r"""out/host-tools/venv/bin/cmake -S tests/sdk_consumer/audio -B out/sdk-audio-consumer -G Ninja \
  -DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++" \
  -DCMAKE_MAKE_PROGRAM="$PWD/out/host-tools/venv/bin/ninja" \
  -DCMAKE_OSX_SYSROOT="$PWD/out/host-tools/macos-sdk" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++ -nostdinc++ -isystem \"$PWD/out/host-tools/libcxx-include\"" \
  -DCMAKE_PREFIX_PATH="$PWD/out/install/macos-clang-development;$PWD/out/conan/macos-clang-development" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
out/host-tools/venv/bin/cmake --build out/sdk-audio-consumer
out/sdk-audio-consumer/ludus_audio_consumer
out/host-tools/venv/bin/ctest --test-dir out/sdk-audio-consumer -R "^audio_transition_timing$" --output-on-failure --no-tests=error
out/host-tools/bin/clang-tidy -p out/sdk-audio-consumer --warnings-as-errors='*' tests/sdk_consumer/audio.cpp
"""),
    Check('persistence-sdk', 'Validate installed persistence and watcher SDK', needs=('sdk-install',),
          run=r"""out/host-tools/venv/bin/cmake -S tests/sdk_consumer/persistence -B out/sdk-persistence-consumer -G Ninja \
  -DCMAKE_CXX_COMPILER="$PWD/out/host-tools/bin/clang++" \
  -DCMAKE_MAKE_PROGRAM="$PWD/out/host-tools/venv/bin/ninja" \
  -DCMAKE_OSX_SYSROOT="$PWD/out/host-tools/macos-sdk" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++ -nostdinc++ -isystem \"$PWD/out/host-tools/libcxx-include\"" \
  -DCMAKE_PREFIX_PATH="$PWD/out/install/macos-clang-development;$PWD/out/conan/macos-clang-development" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
out/host-tools/venv/bin/cmake --build out/sdk-persistence-consumer
out/sdk-persistence-consumer/ludus_persistence_consumer
out/host-tools/bin/clang-tidy -p out/sdk-persistence-consumer --warnings-as-errors='*' tests/sdk_consumer/persistence.cpp
"""),
]
