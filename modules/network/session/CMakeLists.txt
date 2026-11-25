# N2 remains a build-tree module while the transport and session API are qualified.
add_library(ludus_network_session STATIC src/session.cpp)
add_library(Ludus::NetworkSession ALIAS ludus_network_session)
target_include_directories(ludus_network_session PUBLIC src)
target_link_libraries(ludus_network_session PUBLIC
    Ludus::NetworkCore Ludus::FoundationContainers Ludus::FoundationTime)
ludus_apply_project_defaults(ludus_network_session)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_network_session_tests tests/session_tests.cpp)
    target_link_libraries(ludus_network_session_tests PRIVATE Ludus::NetworkSession Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_network_session_tests)
    ludus_enable_test_exceptions(ludus_network_session_tests)
    add_test(NAME ludus_network_session_tests COMMAND ludus_network_session_tests)
    set_tests_properties(ludus_network_session_tests PROPERTIES LABELS "unit;network")
endif()

if(LUDUS_BUILD_NETWORK_NATIVE_PROBE)
    add_subdirectory(native)
endif()
