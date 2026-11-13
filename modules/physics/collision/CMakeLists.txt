add_library(ludus_physics_collision STATIC
    src/collision_world.cpp
    src/convex_hull_cook.cpp
    src/geometry_store.cpp
    src/gjk_simplex.cpp
    src/triangle_mesh_box_cast.cpp
    src/triangle_mesh_convex_cast.cpp
    src/triangle_mesh_cook.cpp
    src/triangle_mesh_raycast.cpp)
add_library(Ludus::PhysicsCollision ALIAS ludus_physics_collision)
set_target_properties(ludus_physics_collision PROPERTIES EXPORT_NAME PhysicsCollision)
target_sources(ludus_physics_collision PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/physics/collision/collision_world.h
    include/ludus/physics/collision/convex_shape.h
    include/ludus/physics/collision/geometry_store.h
    include/ludus/physics/collision/queries.h
    include/ludus/physics/collision/status.h
    include/ludus/physics/collision/triangle_mesh.h)
target_link_libraries(ludus_physics_collision PUBLIC Ludus::FoundationBase Ludus::FoundationContainers
    Ludus::FoundationIdentity Ludus::FoundationMath)
# The watertight ray test relies on exact a*b - c*d antisymmetry, and the
# hierarchy and brute-force traversals must run identical arithmetic.
target_compile_options(ludus_physics_collision PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
ludus_apply_project_defaults(ludus_physics_collision)
install(TARGETS ludus_physics_collision EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_physics_collision_tests tests/triangle_mesh_tests.cpp tests/triangle_mesh_oracle_tests.cpp
        tests/convex_cast_tests.cpp tests/convex_cast_oracle_tests.cpp tests/collision_world_tests.cpp)
    target_link_libraries(ludus_physics_collision_tests PRIVATE Ludus::PhysicsCollision Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_physics_collision_tests)
    ludus_enable_test_exceptions(ludus_physics_collision_tests)
    add_test(NAME ludus_physics_collision_tests COMMAND ludus_physics_collision_tests)
    set_tests_properties(ludus_physics_collision_tests PROPERTIES LABELS "unit;collision")
endif()
# Replaces global operator new to fail chosen allocations; sanitizer runtimes
# replace it too, so sanitizer builds rely on the ordinary suite.
if(LUDUS_BUILD_TESTS AND NOT MSVC AND NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
    add_executable(ludus_physics_collision_allocation_tests tests/triangle_mesh_allocation_tests.cpp)
    target_link_libraries(ludus_physics_collision_allocation_tests PRIVATE Ludus::PhysicsCollision Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_physics_collision_allocation_tests)
    ludus_enable_test_exceptions(ludus_physics_collision_allocation_tests)
    add_test(NAME ludus_physics_collision_allocation_tests COMMAND ludus_physics_collision_allocation_tests)
    set_tests_properties(ludus_physics_collision_allocation_tests PROPERTIES LABELS "unit;collision;alloc")
endif()
