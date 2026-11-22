# Toolchain for the android-clang-* and android-emulator-clang-development presets.
#
# Option A of the mobile toolchain proposal (docs/development/mobile-support-plan.md):
# the pinned upstream Clang compiles against the NDK sysroot and its libc++ headers,
# and LLD links with 16 KB page alignment. The presets select the compiler.
# Inputs: LUDUS_ANDROID_API (provisional until the probes record the minimum API),
# and an explicit NDK/SDK override or the checkout-local NDK installed by init.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    LUDUS_ANDROID_API LUDUS_ANDROID_EMULATOR LUDUS_ANDROID_NDK LUDUS_HOST_TOOLS_PREFIX)
if(NOT LUDUS_ANDROID_API)
    message(FATAL_ERROR "Select an android-* configure preset; LUDUS_ANDROID_API is not set.")
endif()

set(ludus_ndk "${LUDUS_ANDROID_NDK}")
if(ludus_ndk STREQUAL "")
    set(ludus_ndk "$ENV{ANDROID_NDK_HOME}")
endif()
if(ludus_ndk STREQUAL "")
    set(ludus_sdk "$ENV{ANDROID_HOME}")
    if(ludus_sdk STREQUAL "")
        set(ludus_sdk "$ENV{ANDROID_SDK_ROOT}")
    endif()
    if(ludus_sdk STREQUAL "")
        # The toolchain is also loaded inside try_compile, where CMAKE_SOURCE_DIR
        # is the generated test directory. Resolve managed tools from this file.
        set(ludus_sdk "${CMAKE_CURRENT_LIST_DIR}/../../out/host-tools/android-sdk")
        file(READ "${CMAKE_CURRENT_LIST_DIR}/../android_toolchain.json" ludus_android_pins)
        string(JSON ludus_ndk_revision GET "${ludus_android_pins}" ndk)
        set(ludus_ndk "${ludus_sdk}/ndk/${ludus_ndk_revision}")
    else()
        file(GLOB ludus_ndk_revisions LIST_DIRECTORIES true "${ludus_sdk}/ndk/*")
        list(SORT ludus_ndk_revisions COMPARE NATURAL ORDER DESCENDING)
        if(ludus_ndk_revisions)
            list(GET ludus_ndk_revisions 0 ludus_ndk)
        endif()
    endif()
endif()
if(ludus_ndk STREQUAL "" OR NOT EXISTS "${ludus_ndk}/source.properties")
    message(FATAL_ERROR
        "The Android NDK is not installed. Run `./init.sh --cli --android-only` or "
        "set ANDROID_HOME or ANDROID_NDK_HOME to an existing installation.")
endif()
file(REAL_PATH "${ludus_ndk}" ludus_ndk)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(ludus_ndk_host darwin-x86_64)
else()
    set(ludus_ndk_host linux-x86_64)
endif()
set(ludus_ndk_llvm "${ludus_ndk}/toolchains/llvm/prebuilt/${ludus_ndk_host}")
set(ludus_ndk_sysroot "${ludus_ndk_llvm}/sysroot")
set(ludus_android_triple "aarch64-linux-android${LUDUS_ANDROID_API}")

set(CMAKE_SYSTEM_NAME Android)
# CMake's Android platform uses version 1 for external toolchain files that
# select their own compiler. The target triple still declares the Android API.
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(CMAKE_ANDROID_NDK "${ludus_ndk}")
set(CMAKE_C_COMPILER_TARGET "${ludus_android_triple}")
set(CMAKE_CXX_COMPILER_TARGET "${ludus_android_triple}")
set(CMAKE_SYSROOT "${ludus_ndk_sysroot}")
set(CMAKE_AR "${ludus_ndk_llvm}/bin/llvm-ar")
set(CMAKE_RANLIB "${ludus_ndk_llvm}/bin/llvm-ranlib")

# Use the same headers, runtime archives and page alignment as the M0 probe.
set(ludus_android_lld "${LUDUS_HOST_TOOLS_PREFIX}/bin/ld.lld")
if(NOT EXISTS "${ludus_android_lld}")
    message(FATAL_ERROR "The pinned Android linker is missing. Run native setup with `./init.sh`.")
endif()
execute_process(
    COMMAND "${ludus_ndk_llvm}/bin/${ludus_android_triple}-clang++"
        --rtlib=compiler-rt --print-libgcc-file-name
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_VARIABLE ludus_android_builtins
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set(ludus_android_libcxx "${ludus_ndk_sysroot}/usr/lib/aarch64-linux-android/libc++_static.a")
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem \"${ludus_ndk_sysroot}/usr/include/c++/v1\"")
set(ludus_android_link_flags "-nodefaultlibs --ld-path=\"${ludus_android_lld}\" -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${ludus_android_link_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${ludus_android_link_flags}")
set(CMAKE_C_STANDARD_LIBRARIES_INIT "\"${ludus_android_builtins}\" -lc -lm -ldl -llog")
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "\"${ludus_android_libcxx}\" ${CMAKE_C_STANDARD_LIBRARIES_INIT}")

if(LUDUS_ANDROID_EMULATOR)
    set(ludus_runner_platform android-emulator)
else()
    set(ludus_runner_platform android)
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR
    "${CMAKE_CURRENT_LIST_DIR}/../../scripts/mobile-device-runner;--platform;${ludus_runner_platform}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
