# Toolchain for the ios-clang-* and ios-simulator-clang-development presets.
#
# Option A of the mobile toolchain proposal (docs/development/mobile-support-plan.md):
# the pinned upstream Clang compiles against the Xcode SDK and Xcode's linker links.
# The presets select the compiler; this file selects the SDK and the test runner.
# Inputs (set by the presets): LUDUS_IOS_SDK (iphoneos or iphonesimulator) and
# LUDUS_IOS_DEPLOYMENT_TARGET, which is provisional until the probes record the
# minimum OS.
if(NOT LUDUS_IOS_SDK)
    set(LUDUS_IOS_SDK iphoneos)
endif()
if(NOT LUDUS_IOS_DEPLOYMENT_TARGET)
    message(FATAL_ERROR "Select an ios-* configure preset; LUDUS_IOS_DEPLOYMENT_TARGET is not set.")
endif()

execute_process(
    COMMAND xcrun --sdk ${LUDUS_IOS_SDK} --show-sdk-path
    RESULT_VARIABLE ludus_sdk_result
    OUTPUT_VARIABLE ludus_sdk_path
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT ludus_sdk_result EQUAL 0 OR ludus_sdk_path STREQUAL "")
    message(FATAL_ERROR
        "The ${LUDUS_IOS_SDK} SDK is not available. On macOS, run `./init.sh --with-ios` "
        "to prepare Xcode and the iOS SDKs, then run `./scripts/mobile-probe apple-toolchain`. "
        "Command Line Tools alone have no iOS SDK.")
endif()

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_SYSROOT "${ludus_sdk_path}")

if(LUDUS_IOS_SDK STREQUAL "iphonesimulator")
    set(ludus_ios_triple "arm64-apple-ios${LUDUS_IOS_DEPLOYMENT_TARGET}-simulator")
    set(ludus_runner_platform ios-simulator)
else()
    set(ludus_ios_triple "arm64-apple-ios${LUDUS_IOS_DEPLOYMENT_TARGET}")
    set(ludus_runner_platform ios)
endif()
set(CMAKE_C_COMPILER_TARGET "${ludus_ios_triple}")
set(CMAKE_CXX_COMPILER_TARGET "${ludus_ios_triple}")
set(CMAKE_OBJCXX_COMPILER_TARGET "${ludus_ios_triple}")

# Target binaries run only through the device runner, which refuses to run without
# a selected device. Shader compilers and cookers come from the host-tools prefix.
set(CMAKE_CROSSCOMPILING_EMULATOR
    "${CMAKE_CURRENT_LIST_DIR}/../../scripts/mobile-device-runner;--platform;${ludus_runner_platform}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
