add_library(ludus_animation STATIC src/skeleton.cpp src/pose.cpp src/skin_binding.cpp src/inspection.cpp)
add_library(Ludus::Animation ALIAS ludus_animation)
set_target_properties(ludus_animation PROPERTIES EXPORT_NAME Animation)
target_sources(ludus_animation PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/animation/inspection.hpp
    include/ludus/animation/pose.hpp
    include/ludus/animation/skeleton.hpp
    include/ludus/animation/skin_binding.hpp
    include/ludus/animation/status.hpp)
target_link_libraries(ludus_animation PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationHash
    Ludus::FoundationMath)
# Forward kinematics must match FoundationMath Compose exactly in the parity tests.
target_compile_options(ludus_animation PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
ludus_apply_project_defaults(ludus_animation)
install(TARGETS ludus_animation EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_animation_tests tests/skeleton_tests.cpp tests/pose_tests.cpp tests/skin_binding_tests.cpp
        tests/inspection_tests.cpp)
    target_link_libraries(ludus_animation_tests PRIVATE Ludus::Animation Catch2::Catch2WithMain)
    target_compile_options(ludus_animation_tests PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
    ludus_apply_project_defaults(ludus_animation_tests)
    ludus_enable_test_exceptions(ludus_animation_tests)
    add_test(NAME ludus_animation_tests COMMAND ludus_animation_tests)
    set_tests_properties(ludus_animation_tests PROPERTIES LABELS "unit;animation")
endif()
# Replaces global operator new to fail chosen allocations; sanitizer runtimes
# replace it too, so sanitizer builds rely on the ordinary suite.
if(LUDUS_BUILD_TESTS AND NOT MSVC AND NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
    add_executable(ludus_animation_allocation_tests tests/animation_allocation_tests.cpp)
    target_link_libraries(ludus_animation_allocation_tests PRIVATE Ludus::Animation Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_animation_allocation_tests)
    ludus_enable_test_exceptions(ludus_animation_allocation_tests)
    add_test(NAME ludus_animation_allocation_tests COMMAND ludus_animation_allocation_tests)
    set_tests_properties(ludus_animation_allocation_tests PROPERTIES LABELS "unit;animation;alloc")
endif()
