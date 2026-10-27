add_library(ludus_foundation_math STATIC)
add_library(Ludus::FoundationMath ALIAS ludus_foundation_math)

set_target_properties(
    ludus_foundation_math
    PROPERTIES
        OUTPUT_NAME ludus_foundation_math
        EXPORT_NAME FoundationMath
)

target_sources(
    ludus_foundation_math
    PRIVATE
        src/scalar.cpp
        src/color.cpp
        src/vector.cpp
        src/quaternion.cpp
        src/matrix.cpp
        src/transform.cpp
        src/projection.cpp
        src/queries.cpp
        src/ray_origin.cpp
        src/precision.cpp
        src/random.cpp
        src/addressed_random.cpp
        src/batch.cpp
        src/dynamics.cpp
        src/cubic.cpp
        src/patch.cpp
        src/summation.cpp
        src/enclosure.cpp
        src/projected_solid_angle.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS
            include
        FILES
            include/ludus/foundation/math/status.hpp
            include/ludus/foundation/math/scalar.hpp
            include/ludus/foundation/math/vector.hpp
            include/ludus/foundation/math/quaternion.hpp
            include/ludus/foundation/math/matrix.hpp
            include/ludus/foundation/math/summation.hpp
            include/ludus/foundation/math/transform.hpp
            include/ludus/foundation/math/projection.hpp
            include/ludus/foundation/math/geometry.hpp
            include/ludus/foundation/math/queries.hpp
            include/ludus/foundation/math/precision.hpp
            include/ludus/foundation/math/random.hpp
            include/ludus/foundation/math/addressed_random.hpp
            include/ludus/foundation/math/random_domains.hpp
            include/ludus/foundation/math/batch.hpp
            include/ludus/foundation/math/dynamics.hpp
            include/ludus/foundation/math/cubic.hpp
            include/ludus/foundation/math/patch.hpp
            include/ludus/foundation/math/enclosure.hpp
            include/ludus/foundation/math/projected_solid_angle.hpp
)

# Dependency direction: FoundationMath may use FoundationBase (types,
# assertions, compiler macros), never the reverse. Math links nothing else.
target_link_libraries(
    ludus_foundation_math
    PUBLIC
        Ludus::FoundationBase
)

# The checked numerical algorithms require ordinary precise IEEE arithmetic.
# Compile the implementation translation units with contraction disabled and no
# fast math, independent of the preset's global flags (ADR 0010 §4). Inline
# header operators inherit the consumer's own flags, which the module's headers
# reject when fast/finite-only math is detected.
target_compile_options(
    ludus_foundation_math
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
)

ludus_apply_project_defaults(ludus_foundation_math)

install(
    TARGETS ludus_foundation_math
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)

    add_executable(ludus_foundation_math_tests)
    target_sources(
        ludus_foundation_math_tests
        PRIVATE
            tests/scalar_tests.cpp
            tests/vector_tests.cpp
            tests/quaternion_tests.cpp
            tests/matrix_tests.cpp
            tests/transform_tests.cpp
            tests/projection_tests.cpp
            tests/geometry_tests.cpp
            tests/queries_tests.cpp
            tests/precision_tests.cpp
            tests/random_tests.cpp
            tests/addressed_random_tests.cpp
            tests/random_domains_tests.cpp
            tests/batch_tests.cpp
            tests/layout_tests.cpp
            tests/dynamics_tests.cpp
            tests/bezier_tests.cpp
            tests/curves_surfaces_fixture_tests.cpp
            tests/summation_tests.cpp
            tests/enclosure_tests.cpp
            tests/projected_solid_angle_tests.cpp
    )
    target_include_directories(ludus_foundation_math_tests PRIVATE tests src)
    target_link_libraries(
        ludus_foundation_math_tests
        PRIVATE
            Ludus::FoundationMath
            Ludus::FoundationContainers
            Catch2::Catch2WithMain
            # Test-only G0 reference corpus (tests/fixtures/curves_surfaces); Math itself links only Base.
            ludus_geometry_reference_fixtures
    )
    # Tests must observe the same precise FP semantics as the production .cpp so
    # oracle comparisons are meaningful.
    target_compile_options(
        ludus_foundation_math_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
    )
    ludus_apply_project_defaults(ludus_foundation_math_tests)
    ludus_enable_test_exceptions(ludus_foundation_math_tests)

    add_test(NAME ludus_foundation_math_tests COMMAND ludus_foundation_math_tests)
    set_tests_properties(ludus_foundation_math_tests PROPERTIES LABELS "unit;foundation;math")

    # Zero-allocation probe: run representative checked operations through the
    # production path under a malloc wrap and assert no allocation occurs. Runs
    # only without sanitizers (they intercept the allocator themselves), matching
    # the containers module pattern.
    if(NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
        add_executable(ludus_math_alloc_tests)
        target_sources(ludus_math_alloc_tests PRIVATE tests/allocation_tests.cpp)
        target_include_directories(ludus_math_alloc_tests PRIVATE tests)
        target_link_libraries(
            ludus_math_alloc_tests
            PRIVATE
                Ludus::FoundationMath
                Catch2::Catch2WithMain
        )
        ludus_apply_project_defaults(ludus_math_alloc_tests)
        ludus_enable_test_exceptions(ludus_math_alloc_tests)
        add_test(NAME ludus_math_alloc_tests COMMAND ludus_math_alloc_tests)
        set_tests_properties(ludus_math_alloc_tests PROPERTIES LABELS "unit;foundation;math;alloc")
    endif()
endif()

install(FILES shaders/math.slang shaders/projected_solid_angle.slang DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/shaders")
