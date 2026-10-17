# SDK consumer checks are intentionally configured by scripts/install-sdk
# against the installed package, not through this source-tree build.

if(LUDUS_BUILD_TESTS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_subdirectory(quality)
    add_subdirectory(fixtures/curves_surfaces)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_build_contract_tests build_contract/exception_tests.cpp)
    target_link_libraries(ludus_build_contract_tests PRIVATE Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_build_contract_tests)
    ludus_enable_test_exceptions(ludus_build_contract_tests)
    add_test(NAME ludus_build_contract_tests COMMAND ludus_build_contract_tests)
    add_test(NAME ludus_compile_policy
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_contract/verify_compile_policy.py"
            "${PROJECT_BINARY_DIR}/compile_commands.json")
    add_test(NAME ludus_setup_preflight
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_contract/verify_setup_preflight.py"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" "${CMAKE_MAKE_PROGRAM}")
    add_test(NAME ludus_editor_sample_resources
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_contract/verify_editor_sample_resources.py"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
    add_test(NAME ludus_dependency_diagnostics
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_contract/verify_dependency_diagnostics.py"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" "${CMAKE_MAKE_PROGRAM}")
    add_test(NAME ludus_assert_policy
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_contract/verify_assert_policy.py"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}"
            "${LUDUS_BUILD_FLAVOR_ID}" "${LUDUS_ENABLE_ASSERTS}" "${LUDUS_BREAK_ON_CHECK}")
    add_test(NAME ludus_assert_headers
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/assertions/verify_headers.py"
            "${PROJECT_BINARY_DIR}" "${LUDUS_ENABLE_ASSERTS}")
    # Foundational-header architecture gates (ADR 0007):
    #  - every public header compiles standalone (self-sufficiency);
    #  - the foundational headers / public headers respect the include boundary.
    add_test(NAME ludus_header_self_sufficiency
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_contract/verify_header_self_sufficiency.py"
            "${PROJECT_BINARY_DIR}")
    add_test(NAME ludus_pointer_diagnostics
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_contract/verify_pointer_diagnostics.py"
            "${PROJECT_BINARY_DIR}")
    add_test(NAME ludus_foundational_includes
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_foundational_includes.py"
            "${PROJECT_SOURCE_DIR}")
    add_test(NAME ludus_target_detection
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_contract/verify_target_detection.py"
            "${CMAKE_CXX_COMPILER}" "${PROJECT_SOURCE_DIR}")
    # Byte-exact goldens of the still-Python texture cooker (see tests/goldens/scene_pipeline/README.md).
    add_test(NAME ludus_texture_goldens
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/goldens/scene_pipeline"
            -p test_texture_goldens.py -v)
    set_tests_properties(ludus_texture_goldens PROPERTIES LABELS "goldens" TIMEOUT 120)
    set_tests_properties(ludus_build_contract_tests ludus_compile_policy ludus_setup_preflight ludus_editor_sample_resources ludus_dependency_diagnostics ludus_assert_policy ludus_assert_headers
        ludus_header_self_sufficiency ludus_pointer_diagnostics ludus_foundational_includes ludus_target_detection
        PROPERTIES LABELS "build-contract" TIMEOUT 120)
    # One compiler process per public header (~180 on macOS): wall time follows
    # host contention, not the change under test. On a shared 10-core host at
    # load 106-151 the parallel probe took 260 s (101 s CPU). This TIMEOUT only
    # catches a hung compiler; parse-time budgets belong to build_budget.json.
    set_tests_properties(ludus_header_self_sufficiency PROPERTIES TIMEOUT 600)
endif()
