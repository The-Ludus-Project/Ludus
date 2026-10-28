file(READ "${PROJECT_SOURCE_DIR}/config/shader_toolchain.json" shader_lock)
string(JSON slang_version GET "${shader_lock}" slang version)
add_library(ludus_shader_compiler_client STATIC src/client.cpp src/codec.cpp)
add_library(Ludus::ShaderCompilerClient ALIAS ludus_shader_compiler_client)
set_target_properties(ludus_shader_compiler_client PROPERTIES EXPORT_NAME ShaderCompilerClient)
target_compile_features(ludus_shader_compiler_client PUBLIC cxx_std_23)
target_sources(ludus_shader_compiler_client PUBLIC FILE_SET public_headers TYPE HEADERS
    BASE_DIRS include FILES include/ludus/tooling/shader_compiler/compiler.hpp)
target_link_libraries(ludus_shader_compiler_client PUBLIC Ludus::FoundationContainers
    PRIVATE Ludus::FoundationFilesystem Ludus::FoundationParsingJson)
target_compile_definitions(ludus_shader_compiler_client PRIVATE LUDUS_SLANG_VERSION="${slang_version}")
ludus_apply_project_defaults(ludus_shader_compiler_client)
install(TARGETS ludus_shader_compiler_client EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_SHADER_WORKER)
    if(EMSCRIPTEN OR NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
        message(FATAL_ERROR "The compiler worker requires a native Linux/macOS host; clients can still build without it")
    endif()
    set(LUDUS_SLANG_ROOT "${PROJECT_SOURCE_DIR}/out/shader-tools/slang" CACHE PATH "Verified Slang distribution root")
    file(READ "${PROJECT_SOURCE_DIR}/config/shader_toolchain.json" shader_lock)
    string(JSON slang_version GET "${shader_lock}" slang version)
    find_path(slang_include slang.h PATHS "${LUDUS_SLANG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    if(APPLE)
        set(slang_library "${LUDUS_SLANG_ROOT}/lib/libslang-compiler.0.${slang_version}.dylib")
    else()
        set(slang_library "${LUDUS_SLANG_ROOT}/lib/libslang-compiler.so.0.${slang_version}")
    endif()
    if(NOT EXISTS "${slang_library}")
        message(FATAL_ERROR "Missing pinned Slang API library ${slang_library}; bootstrap the verified shader toolchain")
    endif()
    set(worker_identity "")
    foreach(source src/compiler.cpp src/process.cpp src/codec.cpp src/codec.hpp include/ludus/tooling/shader_compiler/compiler.hpp)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_hash)
        string(APPEND worker_identity "${source_hash}")
    endforeach()
    string(SHA256 worker_identity "${worker_identity}")
    file(SHA256 "${slang_library}" slang_library_hash)
    file(GLOB slang_runtime "${LUDUS_SLANG_ROOT}/lib/*.dylib" "${LUDUS_SLANG_ROOT}/lib/*.so*" "${LUDUS_SLANG_ROOT}/lib/*.dll"
        "${LUDUS_SLANG_ROOT}/bin/*.dylib" "${LUDUS_SLANG_ROOT}/bin/*.so*" "${LUDUS_SLANG_ROOT}/bin/*.dll")
    list(SORT slang_runtime)
    set(runtime_inventory "")
    foreach(runtime IN LISTS slang_runtime)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${runtime}")
        file(RELATIVE_PATH relative "${LUDUS_SLANG_ROOT}" "${runtime}")
        file(SHA256 "${runtime}" runtime_hash)
        string(APPEND runtime_inventory "${relative}:${runtime_hash}\n")
    endforeach()
    string(SHA256 runtime_inventory_hash "${runtime_inventory}")
    string(SHA256 worker_identity "${worker_identity}${runtime_inventory_hash}")
    # Only the static closure linked into the optional shared compiler needs PIC.
    set_target_properties(ludus_foundation_base ludus_foundation_memory ludus_foundation_hash
        ludus_foundation_strings ludus_foundation_containers ludus_foundation_filesystem
        ludus_foundation_parsing ludus_foundation_parsing_json ludus_yyjson ludus_shader_compiler_client
        PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(ludus_shader_compiler SHARED src/compiler.cpp src/process.cpp)
    add_library(Ludus::ShaderCompiler ALIAS ludus_shader_compiler)
    set_target_properties(ludus_shader_compiler PROPERTIES EXPORT_NAME ShaderCompiler INSTALL_RPATH "$ORIGIN/ludus-shader-tools")
    install(TARGETS ludus_shader_compiler EXPORT LudusTargets
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    target_include_directories(ludus_shader_compiler SYSTEM PRIVATE "${slang_include}")
    target_link_libraries(ludus_shader_compiler PUBLIC Ludus::ShaderCompilerClient
        PRIVATE Ludus::FoundationStrings Ludus::FoundationFilesystem Ludus::FoundationParsingJson "${slang_library}")
    target_compile_definitions(ludus_shader_compiler PRIVATE LUDUS_SLANG_VERSION="${slang_version}"
        LUDUS_WORKER_IDENTITY="${worker_identity}" LUDUS_SLANG_LIBRARY_HASH="${slang_library_hash}"
        LUDUS_SLANG_INVENTORY_HASH="${runtime_inventory_hash}")
    ludus_apply_project_defaults(ludus_shader_compiler)
    add_executable(ludus-shader-worker "${PROJECT_SOURCE_DIR}/tools/shader-worker/main.cpp")
    target_include_directories(ludus-shader-worker PRIVATE src)
    target_link_libraries(ludus-shader-worker PRIVATE Ludus::ShaderCompiler)
    target_compile_definitions(ludus-shader-worker PRIVATE LUDUS_SLANG_VERSION="${slang_version}")
    # Slang dlopens its SPIR-V optimizer (slang-glslang) beside itself by bare name. A sanitizer
    # runtime intercepts dlopen from the executable, so the loader then searches the executable's
    # run path rather than the Slang library's; without the Slang directory there, Slang silently
    # skips optimization. Name it in the build tree as the install path already does.
    get_filename_component(slang_library_directory "${slang_library}" DIRECTORY)
    set_target_properties(ludus-shader-worker PROPERTIES INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN/../lib/ludus-shader-tools"
        BUILD_RPATH "${slang_library_directory}")
    if(APPLE)
        set_target_properties(ludus_shader_compiler PROPERTIES INSTALL_RPATH "@loader_path/ludus-shader-tools")
        # The pinned upstream macOS Slang API library requires macOS 15.
        target_compile_options(ludus_shader_compiler PUBLIC -mmacosx-version-min=15.0)
        target_link_options(ludus_shader_compiler PUBLIC -mmacosx-version-min=15.0)
        target_compile_options(ludus-shader-worker PRIVATE -mmacosx-version-min=15.0)
        target_link_options(ludus-shader-worker PRIVATE -mmacosx-version-min=15.0)
        set_target_properties(ludus-shader-worker PROPERTIES INSTALL_RPATH "@executable_path/../lib;@executable_path/../lib/ludus-shader-tools")
    endif()
    ludus_apply_project_defaults(ludus-shader-worker)
    install(TARGETS ludus-shader-worker RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES ${slang_runtime} DESTINATION "${CMAKE_INSTALL_LIBDIR}/ludus-shader-tools")
    file(GLOB_RECURSE slang_licenses LIST_DIRECTORIES false "${LUDUS_SLANG_ROOT}/*LICENSE*" "${LUDUS_SLANG_ROOT}/*NOTICE*")
    foreach(license IN LISTS slang_licenses)
        file(RELATIVE_PATH relative "${LUDUS_SLANG_ROOT}" "${license}")
        get_filename_component(parent "${relative}" DIRECTORY)
        install(FILES "${license}" DESTINATION "${CMAKE_INSTALL_DATADIR}/ludus-shader-tools/licenses/${parent}")
    endforeach()
endif()

if(LUDUS_BUILD_TESTS AND TARGET ludus-shader-worker)
    find_package(Catch2 3 REQUIRED CONFIG)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_executable(ludus_shader_compiler_tests tests/compiler_tests.cpp)
    target_link_libraries(ludus_shader_compiler_tests PRIVATE Ludus::ShaderCompiler Catch2::Catch2WithMain)
    # Loads Slang in process, so it needs the worker's run path to Slang's optimizer.
    set_target_properties(ludus_shader_compiler_tests PROPERTIES BUILD_RPATH "${slang_library_directory}")
    ludus_apply_project_defaults(ludus_shader_compiler_tests)
    ludus_enable_test_exceptions(ludus_shader_compiler_tests)
    add_test(NAME ludus_shader_compiler_tests COMMAND ludus_shader_compiler_tests "[shader][compiler]")
    add_test(NAME ludus_shader_worker_protocol COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/shader-worker/test_worker.py" --worker "$<TARGET_FILE:ludus-shader-worker>"
        --client "$<TARGET_FILE:ludus_shader_compiler_tests>")
    set_tests_properties(ludus_shader_compiler_tests ludus_shader_worker_protocol PROPERTIES
        LABELS "unit;shader;tooling" TIMEOUT 120)
endif()
