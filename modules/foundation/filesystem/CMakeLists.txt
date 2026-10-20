add_library(ludus_foundation_filesystem STATIC src/path.cpp src/namespace.cpp src/pack.cpp src/pack_codec.cpp src/async.cpp src/watch.cpp)
add_library(Ludus::FoundationFilesystem ALIAS ludus_foundation_filesystem)
set_target_properties(ludus_foundation_filesystem PROPERTIES EXPORT_NAME FoundationFilesystem)
if(EMSCRIPTEN)
    # Descriptor reads operate on the editor's session MEMFS. Durable publication,
    # host watches and native async I/O remain separate, unavailable services.
    target_sources(ludus_foundation_filesystem PRIVATE src/posix.cpp src/persistence_unsupported.cpp)
elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    target_sources(ludus_foundation_filesystem PRIVATE src/posix.cpp src/persistence_posix.cpp)
    target_compile_definitions(ludus_foundation_filesystem PRIVATE _FILE_OFFSET_BITS=64)
else()
    target_sources(ludus_foundation_filesystem PRIVATE src/unsupported.cpp src/persistence_unsupported.cpp)
endif()
target_sources(ludus_foundation_filesystem PUBLIC FILE_SET public_headers TYPE HEADERS
    BASE_DIRS include FILES
    include/ludus/foundation/filesystem/filesystem.hpp
    include/ludus/foundation/filesystem/namespace.hpp
    include/ludus/foundation/filesystem/pack.hpp
    include/ludus/foundation/filesystem/async.hpp
    include/ludus/foundation/filesystem/persistence.hpp
    include/ludus/foundation/filesystem/watch.hpp)
target_link_libraries(ludus_foundation_filesystem PUBLIC Ludus::FoundationBase PRIVATE Ludus::FoundationContainers)
if(EMSCRIPTEN)
    target_sources(ludus_foundation_filesystem PRIVATE src/async_unsupported.cpp)
elseif(WIN32)
    target_sources(ludus_foundation_filesystem PRIVATE src/async_windows.cpp)
else()
    target_sources(ludus_foundation_filesystem PRIVATE src/async_posix.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(ludus_foundation_filesystem PUBLIC Threads::Threads)
endif()
ludus_apply_project_defaults(ludus_foundation_filesystem)
install(TARGETS ludus_foundation_filesystem EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(pack_fixture "${CMAKE_CURRENT_BINARY_DIR}/generated/pack_fixture.hpp")
    add_custom_command(OUTPUT "${pack_fixture}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/filesystem/generate_pack_fixtures.py" "${pack_fixture}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/filesystem/generate_pack_fixtures.py" "${PROJECT_SOURCE_DIR}/scripts/python/pack_builder.py"
        VERBATIM)
    add_executable(ludus_filesystem_tests tests/path_tests.cpp tests/namespace_tests.cpp tests/pack_tests.cpp "${pack_fixture}")
    target_include_directories(ludus_filesystem_tests PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT EMSCRIPTEN)
        target_sources(ludus_filesystem_tests PRIVATE tests/file_tests.cpp tests/async_tests.cpp tests/persistence_tests.cpp tests/watch_tests.cpp)
        find_package(Threads REQUIRED)
        target_link_libraries(ludus_filesystem_tests PRIVATE Threads::Threads)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN)
        target_sources(ludus_filesystem_tests PRIVATE tests/io_fault_tests.cpp)
        target_compile_definitions(ludus_filesystem_tests PRIVATE _FILE_OFFSET_BITS=64)
        target_link_options(ludus_filesystem_tests PRIVATE -Wl,--wrap=pread64 -Wl,--wrap=_ZnwmRKSt9nothrow_t)
    endif()
    target_link_libraries(ludus_filesystem_tests PRIVATE Ludus::FoundationFilesystem Ludus::FoundationContainers Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_filesystem_tests)
    ludus_enable_test_exceptions(ludus_filesystem_tests)
    add_test(NAME ludus_filesystem_tests COMMAND ludus_filesystem_tests)
    set_tests_properties(ludus_filesystem_tests PROPERTIES LABELS "unit;foundation;filesystem" TIMEOUT 30)
endif()

if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Mach-O lacks GNU --wrap. Compile the same exception-free backend with a
    # private syscall seam; keep production code and the installed SDK untouched.
    add_library(ludus_filesystem_fault_backend STATIC src/path.cpp src/posix.cpp)
    target_include_directories(ludus_filesystem_fault_backend PUBLIC include)
    target_compile_definitions(ludus_filesystem_fault_backend PRIVATE LUDUS_FILESYSTEM_FAULT_TESTING=1)
    target_link_libraries(ludus_filesystem_fault_backend PUBLIC Ludus::FoundationBase PRIVATE Ludus::FoundationContainers)
    ludus_apply_project_defaults(ludus_filesystem_fault_backend)
    add_executable(ludus_filesystem_fault_tests tests/posix_fault_tests.cpp)
    target_include_directories(ludus_filesystem_fault_tests PRIVATE src)
    target_link_libraries(ludus_filesystem_fault_tests PRIVATE ludus_filesystem_fault_backend Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_filesystem_fault_tests)
    ludus_enable_test_exceptions(ludus_filesystem_fault_tests)
    add_test(NAME ludus_filesystem_fault_tests COMMAND ludus_filesystem_fault_tests)
    set_tests_properties(ludus_filesystem_fault_tests PROPERTIES LABELS "unit;foundation;filesystem;macos" TIMEOUT 30)
endif()

if(LUDUS_BUILD_TESTS)
    # Compile only the namespace with a private allocation seam. Native I/O and
    # path validation come from the production archive; nothing is installed.
    add_library(ludus_filesystem_namespace_fault_backend STATIC src/namespace.cpp)
    target_compile_definitions(ludus_filesystem_namespace_fault_backend PRIVATE LUDUS_FILESYSTEM_NAMESPACE_FAULT_TESTING=1)
    target_link_libraries(ludus_filesystem_namespace_fault_backend PUBLIC Ludus::FoundationFilesystem PRIVATE Ludus::FoundationContainers)
    ludus_apply_project_defaults(ludus_filesystem_namespace_fault_backend)
    add_executable(ludus_filesystem_namespace_fault_tests tests/namespace_fault_tests.cpp)
    target_include_directories(ludus_filesystem_namespace_fault_tests PRIVATE src)
    target_link_libraries(ludus_filesystem_namespace_fault_tests PRIVATE ludus_filesystem_namespace_fault_backend Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_filesystem_namespace_fault_tests)
    ludus_enable_test_exceptions(ludus_filesystem_namespace_fault_tests)
    add_test(NAME ludus_filesystem_namespace_fault_tests COMMAND ludus_filesystem_namespace_fault_tests)
    set_tests_properties(ludus_filesystem_namespace_fault_tests PROPERTIES LABELS "unit;foundation;filesystem" TIMEOUT 30)
endif()

if(LUDUS_BUILD_TESTS)
    add_test(NAME ludus_pack_builder_tests COMMAND "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${PROJECT_SOURCE_DIR}/scripts/python" "${Python3_EXECUTABLE}" -m unittest test_pack_builder -v)
    set_tests_properties(ludus_pack_builder_tests PROPERTIES LABELS "unit;foundation;filesystem;tooling" TIMEOUT 30)
    add_test(NAME ludus_filesystem_streaming_fixture_tests
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/filesystem/test_streaming.py")
    set_tests_properties(ludus_filesystem_streaming_fixture_tests PROPERTIES LABELS "foundation;filesystem;tooling" TIMEOUT 30)
endif()

if(LUDUS_BUILD_TESTS)
    add_library(ludus_filesystem_pack_fault_backend STATIC src/pack.cpp)
    target_compile_definitions(ludus_filesystem_pack_fault_backend PRIVATE LUDUS_FILESYSTEM_PACK_FAULT_TESTING=1)
    target_link_libraries(ludus_filesystem_pack_fault_backend PUBLIC Ludus::FoundationFilesystem PRIVATE Ludus::FoundationContainers)
    ludus_apply_project_defaults(ludus_filesystem_pack_fault_backend)
    add_executable(ludus_filesystem_pack_fault_tests tests/pack_fault_tests.cpp "${pack_fixture}")
    target_include_directories(ludus_filesystem_pack_fault_tests PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(ludus_filesystem_pack_fault_tests PRIVATE ludus_filesystem_pack_fault_backend Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_filesystem_pack_fault_tests)
    ludus_enable_test_exceptions(ludus_filesystem_pack_fault_tests)
    add_test(NAME ludus_filesystem_pack_fault_tests COMMAND ludus_filesystem_pack_fault_tests)
    set_tests_properties(ludus_filesystem_pack_fault_tests PROPERTIES LABELS "unit;foundation;filesystem" TIMEOUT 30)
endif()

option(LUDUS_BUILD_FILESYSTEM_FUZZERS "Build isolated instrumented pack/index/codec fuzz target" OFF)
if(LUDUS_BUILD_FILESYSTEM_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR EMSCRIPTEN)
        message(FATAL_ERROR "Filesystem fuzzing requires native Clang/libFuzzer")
    endif()
    # Compile all byte-provider and pack code with coverage and sanitizers;
    # never rely on uninstrumented production reader objects for fuzz evidence.
    add_executable(ludus_filesystem_pack_fuzzer tests/pack_fuzzer.cpp src/pack.cpp src/pack_codec.cpp
        src/path.cpp src/namespace.cpp src/unsupported.cpp)
    target_include_directories(ludus_filesystem_pack_fuzzer PRIVATE include src)
    target_link_libraries(ludus_filesystem_pack_fuzzer PRIVATE Ludus::FoundationBase Ludus::FoundationContainers)
    ludus_apply_project_defaults(ludus_filesystem_pack_fuzzer)
    target_compile_options(ludus_filesystem_pack_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(ludus_filesystem_pack_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

option(LUDUS_BUILD_FILESYSTEM_BENCHMARK "Build the native offset I/O benchmark" OFF)
if(LUDUS_BUILD_FILESYSTEM_BENCHMARK AND NOT EMSCRIPTEN)
    add_executable(ludus_filesystem_benchmark tests/benchmark.cpp tests/streaming_benchmark.cpp)
    target_link_libraries(ludus_filesystem_benchmark PRIVATE Ludus::FoundationFilesystem Ludus::FoundationContainers)
    ludus_apply_project_defaults(ludus_filesystem_benchmark)
    if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
        add_test(NAME ludus_filesystem_streaming_benchmark_tests
            COMMAND "${CMAKE_COMMAND}" -E env "LUDUS_FS_R2_BENCHMARK=$<TARGET_FILE:ludus_filesystem_benchmark>"
                "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/filesystem/test_streaming_native.py")
        set_tests_properties(ludus_filesystem_streaming_benchmark_tests PROPERTIES
            LABELS "foundation;filesystem;benchmark" TIMEOUT 90)
    endif()
endif()

if(LUDUS_BUILD_TESTS AND NOT EMSCRIPTEN)
    add_library(ludus_filesystem_async_fault_backend STATIC src/async.cpp)
    target_compile_definitions(ludus_filesystem_async_fault_backend PRIVATE LUDUS_FILESYSTEM_ASYNC_FAULT_TESTING=1)
    target_link_libraries(ludus_filesystem_async_fault_backend PUBLIC Ludus::FoundationFilesystem)
    ludus_apply_project_defaults(ludus_filesystem_async_fault_backend)
    add_executable(ludus_filesystem_async_fault_tests tests/async_fault_tests.cpp)
    target_include_directories(ludus_filesystem_async_fault_tests PRIVATE src)
    target_link_libraries(ludus_filesystem_async_fault_tests PRIVATE ludus_filesystem_async_fault_backend Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_filesystem_async_fault_tests)
    ludus_enable_test_exceptions(ludus_filesystem_async_fault_tests)
    if(NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
        target_compile_definitions(ludus_filesystem_async_fault_tests PRIVATE LUDUS_FILESYSTEM_ASYNC_ALLOCATION_TESTING=1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(ludus_filesystem_async_fault_tests PRIVATE
            -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join
            -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init)
    endif()
    add_test(NAME ludus_filesystem_async_fault_tests COMMAND ludus_filesystem_async_fault_tests)
    set_tests_properties(ludus_filesystem_async_fault_tests PROPERTIES LABELS "unit;foundation;filesystem;async" TIMEOUT 30)
endif()

if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT EMSCRIPTEN)
    # Compile the production publication implementation with private syscall and
    # allocation seams; neither hooks nor test headers enter the installed SDK.
    add_library(ludus_filesystem_persistence_fault_backend STATIC src/persistence_posix.cpp)
    target_compile_definitions(ludus_filesystem_persistence_fault_backend PRIVATE LUDUS_FILESYSTEM_PERSISTENCE_FAULT_TESTING=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(ludus_filesystem_persistence_fault_backend PUBLIC Ludus::FoundationFilesystem)
    ludus_apply_project_defaults(ludus_filesystem_persistence_fault_backend)
    add_executable(ludus_filesystem_persistence_fault_tests tests/persistence_fault_tests.cpp)
    target_include_directories(ludus_filesystem_persistence_fault_tests PRIVATE src)
    target_link_libraries(ludus_filesystem_persistence_fault_tests PRIVATE ludus_filesystem_persistence_fault_backend Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_filesystem_persistence_fault_tests)
    ludus_enable_test_exceptions(ludus_filesystem_persistence_fault_tests)
    add_test(NAME ludus_filesystem_persistence_fault_tests COMMAND ludus_filesystem_persistence_fault_tests)
    set_tests_properties(ludus_filesystem_persistence_fault_tests PROPERTIES LABELS "unit;foundation;filesystem;persistence" TIMEOUT 30)

    add_library(ludus_filesystem_watch_fault_backend STATIC src/watch.cpp)
    target_compile_definitions(ludus_filesystem_watch_fault_backend PRIVATE LUDUS_FILESYSTEM_WATCH_FAULT_TESTING=1)
    target_link_libraries(ludus_filesystem_watch_fault_backend PUBLIC Ludus::FoundationFilesystem)
    ludus_apply_project_defaults(ludus_filesystem_watch_fault_backend)
    add_executable(ludus_filesystem_watch_fault_tests tests/watch_fault_tests.cpp)
    target_include_directories(ludus_filesystem_watch_fault_tests PRIVATE src)
    target_link_libraries(ludus_filesystem_watch_fault_tests PRIVATE ludus_filesystem_watch_fault_backend Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_filesystem_watch_fault_tests)
    ludus_enable_test_exceptions(ludus_filesystem_watch_fault_tests)
    add_test(NAME ludus_filesystem_watch_fault_tests COMMAND ludus_filesystem_watch_fault_tests)
    set_tests_properties(ludus_filesystem_watch_fault_tests PROPERTIES LABELS "unit;foundation;filesystem;watch" TIMEOUT 30)
endif()

if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT EMSCRIPTEN)
    add_executable(ludus_filesystem_recovery_probe tests/recovery_probe.cpp)
    target_include_directories(ludus_filesystem_recovery_probe PRIVATE src)
    target_link_libraries(ludus_filesystem_recovery_probe PRIVATE ludus_filesystem_persistence_fault_backend Ludus::FoundationContainers)
    ludus_apply_project_defaults(ludus_filesystem_recovery_probe)
    add_test(NAME ludus_filesystem_recovery_oracle_tests
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/filesystem/test_recovery.py")
    set_tests_properties(ludus_filesystem_recovery_oracle_tests PROPERTIES LABELS "foundation;filesystem;persistence" TIMEOUT 30)
    add_test(NAME ludus_filesystem_process_recovery_tests
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/filesystem/recovery.py"
            --probe "$<TARGET_FILE:ludus_filesystem_recovery_probe>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/recovery-process")
    set_tests_properties(ludus_filesystem_process_recovery_tests PROPERTIES
        LABELS "foundation;filesystem;persistence;process-crash" TIMEOUT 180)
endif()
