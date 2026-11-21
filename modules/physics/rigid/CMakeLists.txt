add_library(ludus_physics_rigid STATIC
    src/hull_mass.cpp
    src/mass_properties.cpp
    src/rigid_math.cpp
    src/rigid_world.cpp
    src/rigid_world_step.cpp)
add_library(Ludus::PhysicsRigid ALIAS ludus_physics_rigid)
set_target_properties(ludus_physics_rigid PROPERTIES EXPORT_NAME PhysicsRigid)
target_sources(ludus_physics_rigid PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/physics/rigid/body.h
    include/ludus/physics/rigid/commands.h
    include/ludus/physics/rigid/mass_properties.h
    include/ludus/physics/rigid/rigid_world.h
    include/ludus/physics/rigid/status.h)
target_link_libraries(ludus_physics_rigid PUBLIC Ludus::FoundationBase Ludus::FoundationContainers
    Ludus::FoundationIdentity Ludus::FoundationMath Ludus::PhysicsCollision)
# Replay compares same-build ticks bit for bit; keep multiply-add fusion from depending on optimization choices.
target_compile_options(ludus_physics_rigid PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
ludus_apply_project_defaults(ludus_physics_rigid)
install(TARGETS ludus_physics_rigid EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_physics_rigid_tests tests/mass_properties_tests.cpp tests/rigid_world_tests.cpp
        tests/rigid_world_motion_tests.cpp tests/rigid_world_command_tests.cpp)
    target_link_libraries(ludus_physics_rigid_tests PRIVATE Ludus::PhysicsRigid Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_physics_rigid_tests)
    ludus_enable_test_exceptions(ludus_physics_rigid_tests)
    add_test(NAME ludus_physics_rigid_tests COMMAND ludus_physics_rigid_tests)
    set_tests_properties(ludus_physics_rigid_tests PROPERTIES LABELS "unit;physics")
endif()
# Replaces global operator new to fail chosen allocations; sanitizer runtimes
# replace it too, so sanitizer builds rely on the ordinary suite.
if(LUDUS_BUILD_TESTS AND NOT MSVC AND NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
    add_executable(ludus_physics_rigid_allocation_tests tests/rigid_allocation_tests.cpp)
    target_link_libraries(ludus_physics_rigid_allocation_tests PRIVATE Ludus::PhysicsRigid Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_physics_rigid_allocation_tests)
    ludus_enable_test_exceptions(ludus_physics_rigid_allocation_tests)
    add_test(NAME ludus_physics_rigid_allocation_tests COMMAND ludus_physics_rigid_allocation_tests)
    set_tests_properties(ludus_physics_rigid_allocation_tests PROPERTIES LABELS "unit;physics;alloc")
endif()
