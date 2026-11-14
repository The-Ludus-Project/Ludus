add_library(ludus_gameplay_camera STATIC src/control.cpp src/director.cpp src/evaluation.cpp)
add_library(Ludus::GameplayCamera ALIAS ludus_gameplay_camera)
set_target_properties(ludus_gameplay_camera PROPERTIES EXPORT_NAME GameplayCamera)
target_sources(ludus_gameplay_camera PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/gameplay/camera/camera.hpp
    include/ludus/gameplay/camera/control.hpp
    include/ludus/gameplay/camera/director.hpp
    include/ludus/gameplay/camera/evaluation.hpp)
target_link_libraries(ludus_gameplay_camera PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationMath)
target_compile_options(ludus_gameplay_camera PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
ludus_apply_project_defaults(ludus_gameplay_camera)
install(TARGETS ludus_gameplay_camera EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_gameplay_camera_tests tests/camera_tests.cpp tests/director_tests.cpp tests/rig_tests.cpp)
    target_link_libraries(ludus_gameplay_camera_tests PRIVATE Ludus::GameplayCamera Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_gameplay_camera_tests)
    ludus_enable_test_exceptions(ludus_gameplay_camera_tests)
    add_test(NAME ludus_gameplay_camera_tests COMMAND ludus_gameplay_camera_tests)
    set_tests_properties(ludus_gameplay_camera_tests PROPERTIES LABELS "unit;camera")
endif()
