# Toolchain for the android-clang-* and android-emulator-clang-development presets.
#
# Option A of the mobile toolchain proposal (docs/development/mobile-support-plan.md):
# the pinned upstream Clang compiles against the NDK sysroot and its libc++ headers,
# and LLD links with 16 KB page alignment. The presets select the compiler.
# Inputs: LUDUS_ANDROID_API (provisional until the probes record the minimum API),
# and the NDK from LUDUS_ANDROID_NDK, ANDROID_NDK_HOME or ANDROID_HOME/ndk/<revision>.
# This file is not exercised without an NDK; ./scripts/mobile-probe android-toolchain
# is the evidence that the same flags compile and link.
if(NOT LUDUS_ANDROID_API)
    message(FATAL_ERROR "Select an android-* configure preset; LUDUS_ANDROID_API is not set.")
endif()

set(ludus_ndk "${LUDUS_ANDROID_NDK}")
if(ludus_ndk STREQUAL "")
    set(ludus_ndk "$ENV{ANDROID_NDK_HOME}")
endif()
if(ludus_ndk STREQUAL "" AND NOT "$ENV{ANDROID_HOME}" STREQUAL "")
    file(GLOB ludus_ndk_revisions LIST_DIRECTORIES true "$ENV{ANDROID_HOME}/ndk/*")
    list(SORT ludus_ndk_revisions COMPARE NATURAL ORDER DESCENDING)
    if(ludus_ndk_revisions)
        list(GET ludus_ndk_revisions 0 ludus_ndk)
    endif()
endif()
if(ludus_ndk STREQUAL "" OR NOT EXISTS "${ludus_ndk}/source.properties")
    message(FATAL_ERROR
        "The Android NDK is not installed. Install the command-line tools and an NDK, set ANDROID_HOME "
        "or ANDROID_NDK_HOME, then run `./scripts/mobile-probe android-toolchain`.")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(ludus_ndk_host darwin-x86_64)
else()
    set(ludus_ndk_host linux-x86_64)
endif()
set(ludus_ndk_llvm "${ludus_ndk}/toolchains/llvm/prebuilt/${ludus_ndk_host}")
set(ludus_ndk_sysroot "${ludus_ndk_llvm}/sysroot")
set(ludus_android_triple "aarch64-linux-android${LUDUS_ANDROID_API}")

set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION "${LUDUS_ANDROID_API}")
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(CMAKE_ANDROID_NDK "${ludus_ndk}")
set(CMAKE_C_COMPILER_TARGET "${ludus_android_triple}")
set(CMAKE_CXX_COMPILER_TARGET "${ludus_android_triple}")
set(CMAKE_SYSROOT "${ludus_ndk_sysroot}")

# The NDK's libc++ headers match the NDK, not the pinned Clang, so take them
# explicitly instead of the headers installed beside the pinned compiler.
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem \"${ludus_ndk_sysroot}/usr/include/c++/v1\"")
# 16 KB page alignment (docs: Support 16 KB page sizes). Verify with llvm-readelf -l.
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")

if(LUDUS_ANDROID_EMULATOR)
    set(ludus_runner_platform android-emulator)
else()
    set(ludus_runner_platform android)
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR
    "${CMAKE_CURRENT_LIST_DIR}/../../scripts/mobile-device-runner;--platform;${ludus_runner_platform}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
