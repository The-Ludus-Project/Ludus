include(FetchContent)

# Valve GameNetworkingSockets v1.6.1, BSD-3-Clause. The archive and all upstream
# implementation details are confined to this opt-in development target.
set(game_networking_sockets_revision d534c19aa760df3fb75fd20db13ba1932b8a5463)
FetchContent_Declare(ludus_game_networking_sockets
    URL "https://codeload.github.com/ValveSoftware/GameNetworkingSockets/tar.gz/${game_networking_sockets_revision}"
    URL_HASH SHA256=099d4b0ff1837f0f27c83d4ce05869ec1d32136866f69a3336cc137e951189ab
    SYSTEM EXCLUDE_FROM_ALL)
set(BUILD_STATIC_LIB ON)
set(BUILD_SHARED_LIB OFF)
set(BUILD_TESTS OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_TOOLS OFF)
set(ENABLE_ICE OFF)
set(USE_STEAMWEBRTC OFF)
set(USE_CRYPTO OpenSSL)
set(USE_CRYPTO25519 OpenSSL)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
# Resolve the same Protobuf dependency for GNS and the certificate fixture.
find_package(Protobuf QUIET CONFIG)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
FetchContent_MakeAvailable(ludus_game_networking_sockets)

# The open-source release exposes certificate requests and signed certificates
# publicly, but keeps its trust store and instance factory private. Compile a
# narrow bridge against the pinned internals, with the same exception policy.
target_include_directories(GameNetworkingSockets_s PRIVATE "${ludus_game_networking_sockets_SOURCE_DIR}/src")
set(credential_bridge "${CMAKE_CURRENT_SOURCE_DIR}/gns_credentials.cpp")
target_sources(GameNetworkingSockets_s PRIVATE "${credential_bridge}")
# Keep the Ludus-owned bridge in standard mode even though GNS uses extensions.
set_property(SOURCE "${credential_bridge}" TARGET_DIRECTORY GameNetworkingSockets_s
    APPEND PROPERTY COMPILE_OPTIONS -std=c++23 "$<$<BOOL:${LUDUS_WARNINGS_AS_ERRORS}>:-Werror>")
target_compile_definitions(GameNetworkingSockets_s PRIVATE STEAMNETWORKINGSOCKETS_ALLOW_DYNAMIC_SELFSIGNED_CERTS)
target_compile_options(GameNetworkingSockets_s PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)

add_library(ludus_network_native_transport STATIC native_transport.cpp)
target_include_directories(ludus_network_native_transport PUBLIC .)
target_link_libraries(ludus_network_native_transport PUBLIC Ludus::NetworkSession PRIVATE GameNetworkingSockets_s)
ludus_apply_project_defaults(ludus_network_native_transport)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_network_native_acceptance tests/native_transport_tests.cpp)
    add_dependencies(ludus_network_native_acceptance GameNetworkingSockets_s)
    target_include_directories(ludus_network_native_acceptance SYSTEM PRIVATE
        "${ludus_game_networking_sockets_BINARY_DIR}/src")
    target_link_libraries(ludus_network_native_acceptance PRIVATE
        ludus_network_native_transport GameNetworkingSockets_s Ludus::FoundationThreading Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_network_native_acceptance)
    ludus_enable_test_exceptions(ludus_network_native_acceptance)
    add_test(NAME ludus_network_native_acceptance COMMAND ludus_network_native_acceptance)
    set_tests_properties(ludus_network_native_acceptance PROPERTIES LABELS "network;native-transport" TIMEOUT 30)
endif()
