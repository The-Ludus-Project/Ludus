# Native OIDN render-graph denoising (LUDUS_ENABLE_OIDN).
#
# ./init.sh installs the pinned OIDN release (config/tool_versions.json) under
# LUDUS_OIDN_INIT_PREFIX unless --no-denoiser is given; configuration itself never
# downloads it. Without a saved init choice, the denoiser defaults ON only when a
# complete official-layout package is found there or on the prefix path.
set(LUDUS_OIDN_INIT_PREFIX "${PROJECT_SOURCE_DIR}/out/denoiser/oidn")
# Runtime files named by their install names, and the release's notices. The SDK
# installs both; LudusConfig's ludus_install_denoiser_runtime copies the runtime.
set(LUDUS_OIDN_RUNTIME_LIBRARIES libOpenImageDenoise.2.dylib libOpenImageDenoise_core.2.5.1.dylib
    libOpenImageDenoise_device_metal.2.5.1.dylib libOpenImageDenoise_device_cpu.2.5.1.dylib libtbb.12.dylib)
set(LUDUS_OIDN_LICENSE_FILES LICENSE.txt third-party-programs.txt third-party-programs-oneTBB.txt)

# Set <out_missing> to the first runtime or notice file absent from the package whose
# CMake config directory is <package_directory>, or to an empty string.
function(ludus_find_missing_oidn_file package_directory out_missing)
    cmake_path(GET package_directory PARENT_PATH cmake_directory)
    cmake_path(GET cmake_directory PARENT_PATH library_directory)
    cmake_path(GET library_directory PARENT_PATH prefix)
    set(missing "")
    foreach(file IN LISTS LUDUS_OIDN_RUNTIME_LIBRARIES)
        if(missing STREQUAL "" AND NOT EXISTS "${library_directory}/${file}")
            set(missing "${library_directory}/${file}")
        endif()
    endforeach()
    foreach(file IN LISTS LUDUS_OIDN_LICENSE_FILES)
        if(missing STREQUAL "" AND NOT EXISTS "${prefix}/doc/${file}")
            set(missing "${prefix}/doc/${file}")
        endif()
    endforeach()
    set(${out_missing} "${missing}" PARENT_SCOPE)
endfunction()

set(ludus_oidn_default OFF)
if(NOT DEFINED CACHE{LUDUS_ENABLE_OIDN} AND APPLE AND NOT EMSCRIPTEN)
    set(ludus_oidn_architecture "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_OSX_ARCHITECTURES)
        set(ludus_oidn_architecture "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    if(ludus_oidn_architecture STREQUAL "arm64")
        find_package(OpenImageDenoise 2.5.1 EXACT CONFIG QUIET HINTS "${LUDUS_OIDN_INIT_PREFIX}")
        if(OpenImageDenoise_FOUND)
            ludus_find_missing_oidn_file("${OpenImageDenoise_DIR}" ludus_oidn_missing)
            if(ludus_oidn_missing STREQUAL "")
                set(ludus_oidn_default ON)
            endif()
        endif()
    endif()
endif()
option(LUDUS_ENABLE_OIDN "Enable native OIDN render-graph denoising (./init.sh selects it on Apple silicon)" ${ludus_oidn_default})
if(LUDUS_ENABLE_OIDN)
    if(EMSCRIPTEN OR NOT APPLE)
        message(FATAL_ERROR "LUDUS_ENABLE_OIDN currently requires native Metal on Apple silicon")
    endif()
    find_package(OpenImageDenoise 2.5.1 EXACT CONFIG HINTS "${LUDUS_OIDN_INIT_PREFIX}")
    if(NOT OpenImageDenoise_FOUND)
        message(FATAL_ERROR "LUDUS_ENABLE_OIDN is ON, but OpenImageDenoise 2.5.1 was not found. Run ./init.sh "
            "to install the pinned denoiser, rerun it with --no-denoiser, or configure with -DLUDUS_ENABLE_OIDN=OFF.")
    endif()
    ludus_find_missing_oidn_file("${OpenImageDenoise_DIR}" ludus_oidn_missing)
    if(NOT ludus_oidn_missing STREQUAL "")
        message(FATAL_ERROR "OpenImageDenoise at ${OpenImageDenoise_DIR} lacks ${ludus_oidn_missing}; use the "
            "official 2.5.1 release that ./init.sh installs, or configure with -DLUDUS_ENABLE_OIDN=OFF.")
    endif()
endif()
