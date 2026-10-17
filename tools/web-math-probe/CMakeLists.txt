# The installed-SDK Geometry exercise also runs here so the Wasm toolchain validates
# FoundationGeometry's Certified distance map and surface mesh bound with the enclosure
# helper they rely on.
add_executable(ludus_web_math_probe main.cpp "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/geometry.cpp")
target_link_libraries(ludus_web_math_probe PRIVATE Ludus::FoundationMath Ludus::FoundationGeometry)
# The enclosure oracle is shared with the native FoundationMath tests.
target_include_directories(ludus_web_math_probe PRIVATE "${PROJECT_SOURCE_DIR}/modules/foundation/math/tests")
ludus_apply_project_defaults(ludus_web_math_probe)
# The checked math .cpp files already compile with -ffp-contract=off through the
# library target; the probe only calls them. Keep exceptions off (engine policy).
target_link_options(ludus_web_math_probe PRIVATE
    -fno-exceptions -g0 -g2 -sENVIRONMENT=web,node -sEXIT_RUNTIME=1
    -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1
)
set_target_properties(ludus_web_math_probe PROPERTIES OUTPUT_NAME index SUFFIX ".js")
configure_file(shell.html index.html COPYONLY)

# Runs the wasm numeric corpus through the SDK Node runtime. Browser execution is
# recorded separately in the evidence ledger; Node proves the wasm arithmetic
# runs and matches the frozen answers, not GPU behaviour.
enable_testing()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME ludus_web_math_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_corpus.py"
        "${CMAKE_CROSSCOMPILING_EMULATOR}" "$<TARGET_FILE:ludus_web_math_probe>")
set_tests_properties(ludus_web_math_corpus PROPERTIES LABELS "web;foundation;math" TIMEOUT 120)
