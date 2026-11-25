include("${CMAKE_CURRENT_LIST_DIR}/LudusDependencies.cmake")

# Direct CMake/IDE configuration must record the same validated inputs as the
# Python wrapper. Otherwise a subsequent scripts/build treats that successful
# configure as stale and --fresh discards caller selections such as Slang.
function(ludus_record_macos_configuration)
    get_property(identity GLOBAL PROPERTY LUDUS_CONFIGURED_MACOS_TOOLCHAIN)
    file(WRITE "${CMAKE_BINARY_DIR}/.ludus-macos-toolchain.json" "${identity}\n")
endfunction()
# Run before project(): otherwise CMake's compiler setup reports only a missing
# toolchain path, without explaining how to prepare this checkout/preset.
function(ludus_check_setup)
    if(NOT CMAKE_TOOLCHAIN_FILE)
        ludus_dependency_error("Engine configuration" "initialized build dependencies"
            "No CMAKE_TOOLCHAIN_FILE was selected. Initialize this checkout before configuring it."
            "From the repository root: ./init.sh\n  Then select a Ludus configure preset. Advanced custom setups must supply their own CMAKE_TOOLCHAIN_FILE.")
    endif()

    # Match CMake's relative toolchain lookup: binary directory, then source.
    set(toolchain "${CMAKE_TOOLCHAIN_FILE}")
    cmake_path(ABSOLUTE_PATH toolchain BASE_DIRECTORY "${CMAKE_BINARY_DIR}" NORMALIZE
        OUTPUT_VARIABLE binary_toolchain)
    cmake_path(ABSOLUTE_PATH toolchain BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" NORMALIZE)
    if(EXISTS "${binary_toolchain}")
        set(toolchain "${binary_toolchain}")
    endif()

    set(conan_directory "${CMAKE_SOURCE_DIR}/out/conan")
    cmake_path(IS_PREFIX conan_directory "${toolchain}" NORMALIZE is_conan_toolchain)
    get_filename_component(filename "${toolchain}" NAME)
    if(is_conan_toolchain AND filename STREQUAL "conan_toolchain.cmake")
        get_filename_component(directory "${toolchain}" DIRECTORY)
        get_filename_component(preset "${directory}" NAME)
        if(NOT preset MATCHES "^(linux|macos)-clang-(debug|development|profile|release|asan-ubsan)$")
            return()
        endif()
    elseif(toolchain STREQUAL "${CMAKE_SOURCE_DIR}/out/host-tools/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
        get_filename_component(preset "${CMAKE_BINARY_DIR}" NAME)
        if(NOT preset MATCHES "^web-emscripten-(development|release)$")
            set(preset "web-emscripten-development")
        endif()
    else()
        # A custom toolchain belongs to its caller; init cannot repair it.
        return()
    endif()

    set(command "./init.sh --cli --preset ${preset} --preset-only --locked")
    if(LUDUS_BUILD_TESTS AND NOT preset MATCHES "^web-")
        string(APPEND command " --with-tests")
    endif()
    set(scope_hint "")
    if(NOT preset MATCHES "^web-")
        set(scope_hint "--preset-only prepares only the selected profile; omit it to prepare all native profiles.\n")
    endif()
    if(EXISTS "${toolchain}")
        if(preset MATCHES "^web-")
            return()
        endif()
        set(marker "${CMAKE_SOURCE_DIR}/out/conan/${preset}/.ludus-bootstrap.json")
        set(state_version "")
        if(EXISTS "${marker}")
            file(READ "${marker}" state)
            string(JSON state_version ERROR_VARIABLE state_error GET "${state}" version)
        endif()
        if(NOT state_version STREQUAL "2")
            ludus_dependency_error("Engine configuration" "completed initialization for ${preset}"
                "Generated toolchain exists, but its initialization record is missing, invalid or outdated: ${marker}."
                "From the repository root: ${command}")
        endif()
        if(preset MATCHES "^macos-")
            foreach(pair "sdk;out/host-tools/macos-sdk" "libcxx;out/host-tools/libcxx-include" "compiler;out/host-tools/bin/clang++")
                list(GET pair 0 key)
                list(GET pair 1 relative)
                string(JSON recorded ERROR_VARIABLE identity_error GET "${state}" macos_toolchain "${key}")
                file(REAL_PATH "${CMAKE_SOURCE_DIR}/${relative}" actual)
                if(identity_error OR NOT recorded STREQUAL actual OR NOT EXISTS "${actual}")
                    ludus_dependency_error("Engine configuration" "the initialized macOS toolchain"
                        "macOS ${key} is missing, moved or not validated. Current: '${actual}'; initialized: '${recorded}'."
                        "From the repository root: ${command}")
                endif()
            endforeach()
            foreach(pair "sdk_settings;out/host-tools/macos-sdk/SDKSettings.json" "libcxx_config;out/host-tools/libcxx-include/__config")
                list(GET pair 0 key)
                list(GET pair 1 relative)
                string(JSON recorded ERROR_VARIABLE identity_error GET "${state}" macos_toolchain "${key}")
                set(input "${CMAKE_SOURCE_DIR}/${relative}")
                if(NOT EXISTS "${input}")
                    ludus_dependency_error("Engine configuration" "complete macOS SDK/libc++ metadata"
                        "Missing initialized input: '${input}'." "From the repository root: ${command}")
                endif()
                file(SHA256 "${input}" actual)
                if(identity_error OR NOT recorded STREQUAL actual)
                    ludus_dependency_error("Engine configuration" "current macOS SDK/libc++ metadata"
                        "Initialized input changed: '${input}'." "From the repository root: ${command}")
                endif()
            endforeach()
            string(JSON identity GET "${state}" macos_toolchain)
            set_property(GLOBAL PROPERTY LUDUS_CONFIGURED_MACOS_TOOLCHAIN "${identity}")
            # Deferred work runs only after configuration completes successfully.
            cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL ludus_record_macos_configuration)
        endif()
        return()
    endif()
    message(FATAL_ERROR
        "Ludus setup is missing for ${preset}.\n"
        "Run init before configuring CMake. From the repository root, run:\n\n"
        "  ${command}\n\n"
        "Then rerun CMake Configure.\n${scope_hint}"
        "Missing generated toolchain: ${toolchain}")
endfunction()

# A cached compiler/Ninja path can survive after tools move or are removed.
# Check explicit selections before project() obscures the setup problem.
foreach(variable CMAKE_MAKE_PROGRAM CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
    if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
        ludus_require_tool(${variable} "Build configuration"
            "Restore the selected tool or set ${variable} to its executable. For project-managed tools run ./init.sh from the repository root.")
    endif()
endforeach()

# The luslc-only host tree (scripts/bootstrap-luslc) links no initialized dependencies; it passes
# the pinned host compilers directly, without a Conan toolchain.
if(NOT LUDUS_LUSLC_ONLY)
    ludus_check_setup()
endif()
