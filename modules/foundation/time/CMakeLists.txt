add_library(ludus_foundation_time STATIC)
add_library(Ludus::FoundationTime ALIAS ludus_foundation_time)
set_target_properties(ludus_foundation_time PROPERTIES
    OUTPUT_NAME ludus_foundation_time EXPORT_NAME FoundationTime)

target_sources(ludus_foundation_time PRIVATE src/time.cpp src/timers.cpp)
if(EMSCRIPTEN)
    target_sources(ludus_foundation_time PRIVATE src/clock_web.cpp)
else()
    target_sources(ludus_foundation_time PRIVATE src/clock.cpp)
endif()
target_sources(ludus_foundation_time PUBLIC FILE_SET public_headers TYPE HEADERS
    BASE_DIRS include FILES
        include/ludus/foundation/time/time.hpp
        include/ludus/foundation/time/timers.hpp)
target_link_libraries(ludus_foundation_time PUBLIC Ludus::FoundationBase)
ludus_apply_project_defaults(ludus_foundation_time)
install(TARGETS ludus_foundation_time EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    find_package(Threads REQUIRED)
    add_executable(ludus_foundation_time_tests tests/time_tests.cpp)
    target_link_libraries(ludus_foundation_time_tests PRIVATE Ludus::FoundationThreading
        Ludus::FoundationTime Catch2::Catch2WithMain Threads::Threads)
    ludus_apply_project_defaults(ludus_foundation_time_tests)
    ludus_enable_test_exceptions(ludus_foundation_time_tests)
    add_test(NAME ludus_foundation_time_tests COMMAND ludus_foundation_time_tests)
    set_tests_properties(ludus_foundation_time_tests PROPERTIES LABELS "unit;foundation;time")
endif()
