cmake_minimum_required(VERSION 3.29)

include(cmake/EngineSetupPreflight.cmake)

set(LUDUS_ENGINE_PROJECT_NAME "Ludus")
set(LUDUS_ENGINE_EXPORT_NAMESPACE "Ludus::")
set(LUDUS_ENGINE_PACKAGE_NAME "Ludus")

project(${LUDUS_ENGINE_PROJECT_NAME} VERSION 0.1.0 LANGUAGES CXX)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Enable in the common parent scope for both Platform and GraphicsRhi.
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Ludus does not use C++20 named modules. Left on, CMake (CMP0155) runs
# clang-scan-deps on every C++ source and collates each target before it
# compiles; see docs/development/build-profiling.md. Turn this back on only
# when the codebase adopts modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(EngineOptions)
include(EngineCompilerWarnings)
include(EngineSanitizers)
include(EngineTargets)
include(EnginePch)
include(LudusShaders)
include(LudusRenderGraph)
include(LudusReflection)
include(EngineSdkIdentity)

option(LUDUS_BUILD_TESTS "Build Ludus tests" ON)
option(LUDUS_BUILD_SHADER_WORKER "Build the optional native Slang compiler library and headless worker" OFF)
option(LUDUS_SHADER_WORKER_ONLY "Configure only the headless shader tooling closure" OFF)
option(LUDUS_BUILD_SMOKE_APP "Build the smoke executable" ON)
option(LUDUS_BUILD_WORLD_DEMO "Build the graphical game world reference (requires shader tools)" OFF)
option(LUDUS_BUILD_SHADER_PROBE "Build the isolated offline shader feasibility probe" OFF)
option(LUDUS_BUILD_LUAU_PROBE "Build the private S0 Luau feasibility probe (requires scripts/luau-probe bootstrap)" OFF)
option(LUDUS_BUILD_ICU_PROBE "Build the private L0 ICU4C localization backend probe (requires scripts/icu-probe bootstrap)" OFF)
option(LUDUS_BUILD_UNICODE "Build Ludus::Unicode and ICU-backed localization formatting (requires scripts/icu-probe bootstrap)" OFF)
option(LUDUS_BUILD_SCRIPT_SESSION "Build the experimental S2 Luau debug/replacement session" OFF)
option(LUDUS_BUILD_VISUAL_SEQUENCE "Build the private S3 visual sequence authoring bridge" OFF)
option(LUDUS_BUILD_BEHAVIOR "Build and install the opt-in S4 C++/Luau behavior provider" OFF)
option(LUDUS_BUILD_WEB_SAMPLES "Build precompiled browser editor sample players" OFF)
option(LUDUS_BUILD_BEHAVIOR_ACCEPTANCE "Build S4 project-owned acceptance examples" OFF)
option(LUDUS_BUILD_SCRIPT_INTERACTION "Build the experimental S1 native/Luau headless interaction" OFF)
# The Editor workspace (apps/editor) is an OPTIONAL developer tool, OFF by
# default. When OFF, configure/build/install and browser targets must not search
# for, link, or export Qt. Qt discovery happens only inside the enabled app
# subdirectory (see apps/editor/CMakeLists.txt), never globally. See
# .kiro/specs/editor-workspace/{requirements,design}.md (E01/E02).
option(LUDUS_BUILD_EDITOR "Build the optional Qt editor workspace (apps/editor)" OFF)
option(LUDUS_BUILD_WEB_PROBES "Build browser feasibility probes" ON)
include(EngineInitOptions)
include(EngineEditorOptions)
if(LUDUS_BUILD_BEHAVIOR)
    include(LudusBehaviorProfile)
    ludus_behavior_admit_profile("${PROJECT_SOURCE_DIR}/config/behavior_profiles.json" LUDUS_BEHAVIOR_TARGET_PROFILE)
endif()
if(EMSCRIPTEN)
    # Native Catch2 test targets are not browser executables.
    if(LUDUS_BUILD_TESTS)
        message(FATAL_ERROR "Use a web-emscripten preset; native tests are not browser targets")
    endif()
endif()
option(LUDUS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(LUDUS_ENABLE_COVERAGE "Enable coverage instrumentation" OFF)

# ccache defaults on when the environment requests it (e.g. CI exports
# LUDUS_ENABLE_CCACHE=1), otherwise off for plain local builds.
if(DEFINED ENV{LUDUS_ENABLE_CCACHE} AND NOT "$ENV{LUDUS_ENABLE_CCACHE}" STREQUAL "" AND NOT "$ENV{LUDUS_ENABLE_CCACHE}" STREQUAL "0")
    set(LUDUS_ENABLE_CCACHE_DEFAULT ON)
else()
    set(LUDUS_ENABLE_CCACHE_DEFAULT OFF)
endif()

option(LUDUS_ENABLE_CCACHE "Use ccache when available" ${LUDUS_ENABLE_CCACHE_DEFAULT})

# Opt-in reuse of ccache results between checkouts at different paths, such as
# concurrent agent worktrees (LUDUS_CCACHE_SHARE_CHECKOUTS=1). Off by default:
# it changes __FILE__ and debug-info paths; see docs/development/building.md.
if(DEFINED ENV{LUDUS_CCACHE_SHARE_CHECKOUTS} AND NOT "$ENV{LUDUS_CCACHE_SHARE_CHECKOUTS}" STREQUAL "" AND NOT "$ENV{LUDUS_CCACHE_SHARE_CHECKOUTS}" STREQUAL "0")
    set(LUDUS_CCACHE_SHARE_CHECKOUTS_DEFAULT ON)
else()
    set(LUDUS_CCACHE_SHARE_CHECKOUTS_DEFAULT OFF)
endif()

option(LUDUS_CCACHE_SHARE_CHECKOUTS "Let ccache reuse objects compiled in checkouts at other paths" ${LUDUS_CCACHE_SHARE_CHECKOUTS_DEFAULT})
option(LUDUS_ENABLE_TIME_TRACE "Emit Clang -ftime-trace per-TU compile profiles" OFF)

if(DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "")
    set(LUDUS_WARNINGS_AS_ERRORS_DEFAULT ON)
else()
    set(LUDUS_WARNINGS_AS_ERRORS_DEFAULT OFF)
endif()

option(LUDUS_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" ${LUDUS_WARNINGS_AS_ERRORS_DEFAULT})
option(LUDUS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LUDUS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

include(EngineBuildFlavor)
ludus_configure_project_options(ludus_project_options)
ludus_configure_project_warnings(ludus_project_warnings)
ludus_configure_project_sanitizers(ludus_project_options)

if(LUDUS_ENABLE_CCACHE)
    find_program(LUDUS_CCACHE_PROGRAM ccache)
    if(LUDUS_CCACHE_PROGRAM)
        message(STATUS "Using ccache: ${LUDUS_CCACHE_PROGRAM}")
        set(ludus_ccache_launcher "${LUDUS_CCACHE_PROGRAM}")
        if(LUDUS_CCACHE_SHARE_CHECKOUTS)
            # CMake can spell the checkout through a symlink (it always keeps
            # macOS /tmp for /private/tmp) while init's Conan toolchain uses the
            # resolved path. base_dir then lists both spellings, and PWD names
            # the build directory as CMake spells it: ccache relativizes paths
            # against $PWD when it is the working directory, and otherwise
            # against the resolved directory, which would route every
            # /tmp-spelled path through / and this checkout's own name.
            file(REAL_PATH "${PROJECT_SOURCE_DIR}" ludus_real_source_dir)
            set(ludus_ccache_base_dir "${PROJECT_SOURCE_DIR}")
            set(ludus_ccache_minimum_version "4.8")
            set(ludus_ccache_environment "")
            if(NOT ludus_real_source_dir STREQUAL PROJECT_SOURCE_DIR)
                string(APPEND ludus_ccache_base_dir ":${ludus_real_source_dir}")
                set(ludus_ccache_minimum_version "4.12")
                find_program(LUDUS_ENV_PROGRAM env)
                if(LUDUS_ENV_PROGRAM)
                    set(ludus_ccache_environment "${LUDUS_ENV_PROGRAM}" "PWD=${CMAKE_BINARY_DIR}")
                endif()
            endif()
            execute_process(COMMAND "${LUDUS_CCACHE_PROGRAM}" --version
                OUTPUT_VARIABLE ludus_ccache_version_text ERROR_QUIET)
            string(REGEX MATCH "version ([0-9]+\\.[0-9]+)" ludus_ccache_version_match "${ludus_ccache_version_text}")
            if("${CMAKE_MATCH_1}" VERSION_LESS ludus_ccache_minimum_version)
                message(WARNING "LUDUS_CCACHE_SHARE_CHECKOUTS needs ccache ${ludus_ccache_minimum_version} or newer "
                    "for base_dir=${ludus_ccache_base_dir}; found '${ludus_ccache_version_match}', so results stay "
                    "specific to this checkout")
            else()
                # base_dir rewrites paths inside this checkout relative to the
                # build directory, so other checkouts hash the same command line.
                # hash_dir=false lets -g objects match too; a reused object's
                # debug info names the build directory that first compiled it.
                list(APPEND ludus_ccache_launcher "base_dir=${ludus_ccache_base_dir}" "hash_dir=false"
                    "absolute_paths_in_stderr=true")
                list(PREPEND ludus_ccache_launcher ${ludus_ccache_environment})
                message(STATUS "ccache shares results with other checkouts (base_dir=${ludus_ccache_base_dir})")
            endif()
        endif()
        # Wrap compilation only. ccache is a compiler cache, not a linker, so it
        # must not be attached to the link rule (doing so corrupts link commands).
        set(CMAKE_C_COMPILER_LAUNCHER "${ludus_ccache_launcher}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${ludus_ccache_launcher}" CACHE STRING "" FORCE)
    else()
        message(WARNING "LUDUS_ENABLE_CCACHE is ON, but ccache was not found")
    endif()
else()
    # The launcher above is a forced cache entry. Drop it when ccache is turned
    # off so an existing build tree stops using ccache without --fresh.
    foreach(ludus_language IN ITEMS C CXX)
        if(LUDUS_CCACHE_PROGRAM AND CMAKE_${ludus_language}_COMPILER_LAUNCHER)
            list(FIND CMAKE_${ludus_language}_COMPILER_LAUNCHER "${LUDUS_CCACHE_PROGRAM}" ludus_ccache_index)
            if(ludus_ccache_index GREATER_EQUAL 0)
                unset(CMAKE_${ludus_language}_COMPILER_LAUNCHER CACHE)
            endif()
        endif()
    endforeach()
endif()

# Gameplay compatibility identity comes from the same actual SDK inputs.
set(LUDUS_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(LUDUS_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
find_package(Git QUIET)
set(LUDUS_GIT_REVISION "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE LUDUS_GIT_REVISION_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(LUDUS_GIT_REVISION_OUTPUT)
        set(LUDUS_GIT_REVISION "${LUDUS_GIT_REVISION_OUTPUT}")
    endif()
    # The installed manifest, build metadata and Editor setup checks compare this
    # revision. Re-run configure when HEAD moves (commit, pull, checkout) so a
    # plain `cmake --build` or IDE build after a pull cannot install an engine
    # labelled with the previous revision. Worktrees keep HEAD in their own
    # git directory, so ask git for each path instead of assuming `.git/`.
    foreach(LUDUS_GIT_HEAD_RELATIVE_PATH IN ITEMS HEAD logs/HEAD packed-refs)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse --path-format=absolute --git-path
                    "${LUDUS_GIT_HEAD_RELATIVE_PATH}"
            OUTPUT_VARIABLE LUDUS_GIT_HEAD_PATH
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(LUDUS_GIT_HEAD_PATH AND EXISTS "${LUDUS_GIT_HEAD_PATH}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LUDUS_GIT_HEAD_PATH}")
        endif()
    endforeach()
endif()

set(LUDUS_GAME_COMPATIBILITY_IDENTITY
    "${LUDUS_GIT_REVISION}|${LUDUS_SDK_VARIANT}|${LUDUS_CXX_RUNTIME_ABI}|${LUDUS_COMPILER_ID} ${LUDUS_COMPILER_VERSION}|${LUDUS_TARGET_TRIPLE}|abi-1.1")

configure_file(cmake/CheckSdkVariant.cmake.in "${PROJECT_BINARY_DIR}/cmake/CheckSdkVariant.cmake" @ONLY)
install(SCRIPT "${PROJECT_BINARY_DIR}/cmake/CheckSdkVariant.cmake" ALL_COMPONENTS)

include(EngineDependencyPreflight)

include(EngineTestDiagnostics)

add_subdirectory(modules/foundation/base)
if(LUDUS_SHADER_WORKER_ONLY)
    if(EMSCRIPTEN OR NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
        message(FATAL_ERROR "The shader worker requires a native Linux/macOS host")
    endif()
    set(LUDUS_BUILD_SHADER_WORKER ON)
    add_subdirectory(modules/foundation/memory)
    add_subdirectory(modules/foundation/hash)
    add_subdirectory(modules/foundation/strings)
    add_subdirectory(modules/foundation/containers)
    add_subdirectory(modules/foundation/filesystem)
    add_subdirectory(third_party/yyjson)
    add_subdirectory(modules/foundation/parsing)
    add_subdirectory(modules/tooling/shader_compiler)
    configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/LudusShaderToolsConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/LudusShaderToolsConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LudusShaderTools")
    install(FILES "${PROJECT_BINARY_DIR}/LudusShaderToolsConfig.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LudusShaderTools")
    install(EXPORT LudusTargets NAMESPACE Ludus:: FILE LudusShaderToolsTargets.cmake
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LudusShaderTools")
    return()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Windows builds the FoundationThreading closure (threading.md, T3) and the
    # Direct3D 12 bring-up closure: Platform's Win32 window and the RHI
    # (docs/architecture/windows-direct3d12.md). Other modules have no Windows
    # backend yet and the SDK is not installed there. Keep the list in dependency
    # order and closed over its Ludus::* links: FoundationProfiling links
    # FoundationStrings, which links FoundationHash; Platform links Input, Logging
    # and Identity (through PlatformEntropy); the RHI links Math and Threading.
    # scripts/python/test_ci.py checks this list against the modules' links.
    foreach(module IN ITEMS memory containers hash strings time profiling threading identity math logging)
        add_subdirectory(modules/foundation/${module})
    endforeach()
    add_subdirectory(modules/input)
    add_subdirectory(modules/platform)
    add_subdirectory(modules/graphics/rhi)
    return()
endif()
# Portable bounded networking primitives are shared by native and web targets.
add_subdirectory(modules/network/core)
add_subdirectory(modules/foundation/memory)
add_subdirectory(modules/foundation/hash)
# FoundationIdentity depends only on FoundationBase and builds for native and web.
add_subdirectory(modules/foundation/identity)
add_subdirectory(modules/foundation/strings)
# Opt-in pinned ICU4C owner; ordinary configure/build/install stay offline and ICU-free.
if(LUDUS_BUILD_UNICODE)
    add_subdirectory(modules/foundation/unicode)
endif()
add_subdirectory(modules/localization)
add_subdirectory(modules/foundation/containers)
add_subdirectory(modules/foundation/filesystem)
add_subdirectory(modules/foundation/threading)
add_subdirectory(tools/container-bench)
add_subdirectory(modules/gameplay/world)
# FoundationMath depends only on FoundationBase. It is added before any consumer
# and outside the native-only branch so native and browser targets share it.
add_subdirectory(modules/foundation/math)
# FoundationGeometry (ADR 0024) depends on Base, Math and Containers only.
add_subdirectory(modules/foundation/geometry)
add_subdirectory(modules/gameplay/camera)
add_subdirectory(modules/physics/fluid)
add_subdirectory(modules/foundation/time)
add_subdirectory(modules/foundation/logging)
add_subdirectory(modules/foundation/profiling)
add_subdirectory(tools/memory-bench)
add_subdirectory(tools/ltc-fit)
# Runtime Ui has no Platform/RHI/Text dependency and builds on both toolchains.
add_subdirectory(modules/ui)
# Input is configured before Platform: Platform depends on Input for the
# normalized keyboard record/sink vocabulary (one-way dependency).
add_subdirectory(modules/input)
add_subdirectory(apps/world_demo)
# Private pinned miniaudio C dependency (locked source bootstrap, hash-verified
# at configure time). Added before the audio module, which links it PRIVATE. It
# deliberately enables the C language for its own targets; the root project
# otherwise declares CXX only (audio spec design section 2, decision log A0).
add_subdirectory(third_party/miniaudio)
# Audio depends only on FoundationBase/Containers/Logging and the private
# miniaudio dependency; it does not depend on Platform/RHI/Input/Text. It builds
# for both native and web targets (same production renderer; design section 1).
add_subdirectory(modules/audio)
add_subdirectory(third_party/yyjson)
add_subdirectory(modules/foundation/parsing)
add_subdirectory(modules/tooling/shader_compiler)
add_subdirectory(modules/foundation/reflection)
add_subdirectory(modules/foundation/serialization)
# Native game modules are shared libraries that link the installed reflection
# and serialization codecs statically. Their whole static closure must be
# position-independent, whether or not the Behavior provider is built.
if(NOT EMSCRIPTEN)
    set_target_properties(ludus_foundation_base ludus_foundation_memory ludus_foundation_math
        ludus_foundation_parsing ludus_foundation_parsing_json ludus_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
add_subdirectory(modules/foundation/config)
add_subdirectory(modules/runtime/configuration)
if(NOT EMSCRIPTEN)
    add_subdirectory(tools/configuration)
endif()
add_subdirectory(modules/content)
# LUSCN cooked scene format contract shared by SceneCook, the Editor and Resources.
add_subdirectory(modules/content/scene_format)
add_subdirectory(modules/content/render_graph_format)
# LUSTR cooked string dictionary contract, validating reader and deterministic writer.
add_subdirectory(modules/content/string_dictionary_format)
add_subdirectory(modules/content/string_dictionary)
# Versioned UUID authoring records and the one-time World ID migration manifest.
add_subdirectory(modules/content/identity_records)
# Headless procedural scatter kernel (P0); Foundation-only, native and web.
add_subdirectory(modules/content/procedural/environment)
# Scene cooker (pbrt-reference-v1). The library builds for every target; its
# file I/O reports Unsupported where Ludus::Content has no native file access,
# and the ludus-cook CLI plus golden tests are native-host only.
# Private pinned stb_image (PNG/JPEG/Radiance decoding for the best-effort scene
# importers) and MikkTSpace (their tangent generation).
add_subdirectory(third_party/stb)
add_subdirectory(third_party/mikktspace)
add_subdirectory(modules/content/scene_cook)
add_subdirectory(modules/audio/content)
add_subdirectory(modules/runtime/resources)

# Ludus::GameApi is a header-only INTERFACE target defining the gameplay module
# ABI (project-live-reload design section 2). It links no engine implementation,
# so it is available to both native and browser configures; the native GameHost
# runtime library and the loader live under the native branch below.
add_subdirectory(modules/runtime/game_api)
add_subdirectory(modules/runtime/property_binding)

if(EMSCRIPTEN)
    add_subdirectory(modules/platform)
    add_subdirectory(modules/graphics/rhi)
    add_subdirectory(modules/graphics/renderer)
    add_subdirectory(modules/runtime/shader_development)
    add_subdirectory(modules/graphics/text)
    # NOTE: Ludus::Text is intentionally NOT built for the Emscripten target yet.
    # Its FreeType/HarfBuzz dependencies are resolved natively through Conan; the
    # separate pinned web source-bootstrap for those libraries is still pending
    # (see .kiro/specs/text-font-rendering/tasks.md F0 "web source-bootstrap
    # pending"). Add it back here once that web dependency path exists, so the
    # browser configure does not fail finding freetype/harfbuzz.
    if(LUDUS_BUILD_WEB_PROBES)
        add_subdirectory(tools/web-rhi-probe)
        add_subdirectory(tools/web-frame-probe)
        add_subdirectory(tools/web-platform-probe)
        add_subdirectory(tools/web-foundation-probe)
        add_subdirectory(tools/web-math-probe)
        add_subdirectory(tools/web-localization-probe)
        add_subdirectory(tools/web-fluid-probe)
        add_subdirectory(tools/web-observability-probe)
    endif()
    if(LUDUS_BUILD_SMOKE_APP)
        add_subdirectory(apps/smoke)
    endif()
else()
    add_subdirectory(modules/platform)
    add_subdirectory(modules/graphics/rhi)
    add_subdirectory(modules/graphics/renderer)
    add_subdirectory(modules/runtime/shader_development)
    add_subdirectory(modules/text)
    add_subdirectory(modules/graphics/text)

    # Qt-free static GameHost runtime library and loader (project-live-reload
    # design section 2). Native-only: it uses dlopen and the Platform/RHI
    # session. The editor never links it; a project-owned host executable does.
    add_subdirectory(modules/runtime/game_host)

    # Startup-only development diagnostic integration (depends on Base; Base does
    # not depend on it). Provides InitializeDiagnosticSession and the Python helper.
    add_subdirectory(tools/diagnostics)

    if(LUDUS_BUILD_SMOKE_APP)
        add_subdirectory(apps/smoke)
        add_subdirectory(apps/content_pack)
        add_subdirectory(apps/audio_content_demo)
        add_subdirectory(apps/input_demo)
    endif()

    # Reference GameHost executable (project-live-reload). Built alongside the
    # smoke apps; it links Ludus::GameHost and no Qt.
    add_subdirectory(apps/game_host)

    if(LUDUS_BUILD_TESTS)
        enable_testing()
    endif()

    add_subdirectory(tests)
endif()

# Editor-OFF builds never discover Qt; browser and native shells share widgets.
if(LUDUS_BUILD_EDITOR)
    add_subdirectory(apps/editor)
endif()

if(LUDUS_BUILD_SHADER_PROBE)
    add_subdirectory(tools/shader-probe)
endif()

if(LUDUS_BUILD_LUAU_PROBE)
    add_subdirectory(tools/luau-probe)
endif()

if(LUDUS_BUILD_ICU_PROBE)
    add_subdirectory(tools/icu-probe)
endif()

if(LUDUS_BUILD_SCRIPT_INTERACTION OR LUDUS_BUILD_SCRIPT_SESSION OR LUDUS_BUILD_VISUAL_SEQUENCE OR LUDUS_BUILD_BEHAVIOR)
    add_subdirectory(modules/runtime/scripting)
endif()
if(LUDUS_BUILD_BEHAVIOR_ACCEPTANCE AND NOT LUDUS_BUILD_BEHAVIOR)
    message(FATAL_ERROR "Behavior acceptance requires LUDUS_BUILD_BEHAVIOR=ON")
endif()
if(NOT LUDUS_BUILD_BEHAVIOR)
    configure_file(cmake/LudusBehaviorPrune.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/LudusBehaviorPrune.cmake" @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/LudusBehaviorPrune.cmake")
endif()
if(LUDUS_BUILD_BEHAVIOR)
    add_subdirectory(modules/runtime/behavior)
    if(LUDUS_BUILD_BEHAVIOR_ACCEPTANCE)
        add_subdirectory(tools/script-provider)
    endif()
endif()
if(LUDUS_BUILD_WEB_SAMPLES)
    if(NOT EMSCRIPTEN OR NOT LUDUS_BUILD_BEHAVIOR)
        message(FATAL_ERROR "Web sample players require Emscripten and LUDUS_BUILD_BEHAVIOR=ON")
    endif()
    add_subdirectory(apps/web_samples)
endif()
if(LUDUS_BUILD_SCRIPT_INTERACTION OR LUDUS_BUILD_SCRIPT_SESSION OR LUDUS_BUILD_VISUAL_SEQUENCE)
    add_subdirectory(tools/script-interaction)
endif()
if(LUDUS_BUILD_SCRIPT_SESSION OR LUDUS_BUILD_VISUAL_SEQUENCE)
    add_subdirectory(tools/script-session)
endif()
if(LUDUS_BUILD_VISUAL_SEQUENCE)
    add_subdirectory(tools/visual-sequence)
endif()

install(
    EXPORT LudusTargets
    NAMESPACE ${LUDUS_ENGINE_EXPORT_NAMESPACE}
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${LUDUS_ENGINE_PACKAGE_NAME}"
)


configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/LudusConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/LudusConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${LUDUS_ENGINE_PACKAGE_NAME}"
)

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/cmake/LudusConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# --- Redistributable dependency inventory (P02/P03/P11) ----------------------
# Public exported components (one per installed Ludus:: module target).
foreach(_component
    FoundationBase FoundationMemory FoundationHash FoundationIdentity FoundationStrings FoundationContainers FoundationThreading FoundationFilesystem FoundationParsing FoundationParsingJson FoundationMath FoundationGeometry FoundationTime FoundationLogging ShaderCompilerClient
    FoundationProfiling FoundationThreadingProfiling FoundationReflection FoundationSerialization FoundationConfig FoundationConfigJson RuntimeConfiguration GamePropertyBinding NetworkCore Input Audio Content SceneFormat RenderGraphFormat StringDictionaryFormat StringDictionary StringDictionaryWriter SceneCook AudioContent Resources GameplayWorld GameplayCamera GameApi PhysicsFluid Ui Localization)
    ludus_sdk_register_component(${_component})
    ludus_sdk_register_feature(${_component})
endforeach()
if(LUDUS_BUILD_BEHAVIOR)
    ludus_sdk_register_component(Behavior)
    ludus_sdk_register_feature(Behavior)
    ludus_sdk_register_dependency(Luau "1eca9fda3e4753a1592000f6cfdf659aaa778b7d-reviewed-loader-v1" "MIT" bundled)
endif()
if(LUDUS_BUILD_UNICODE)
    # Opt-in pinned ICU4C closure behind Ludus::Unicode (docs/modules/foundation/unicode.md).
    ludus_sdk_register_component(Unicode)
    ludus_sdk_register_feature(Unicode)
    ludus_sdk_register_dependency(ICU4C "78.3" "Unicode-3.0" bundled)
endif()
if(NOT EMSCRIPTEN)
    foreach(_component Platform PlatformEntropy PlatformPacing GraphicsRhi GraphicsRenderer GraphicsText Text GameHost ShaderDevelopment)
        ludus_sdk_register_component(${_component})
        ludus_sdk_register_feature(${_component})
    endforeach()
else()
    foreach(_component Platform PlatformEntropy PlatformPacing GraphicsRhi GraphicsRenderer GraphicsText)
        ludus_sdk_register_component(${_component})
        ludus_sdk_register_feature(${_component})
    endforeach()
endif()

if(LUDUS_BUILD_SHADER_WORKER)
    ludus_sdk_register_component(ShaderCompiler)
    ludus_sdk_register_feature(ShaderCompiler)
    ludus_sdk_register_dependency(Slang "2026.1.2" "Apache-2.0-WITH-LLVM-exception" bundled)
endif()

# Redistributable static link-closure dependencies. These are the libraries the
# FINAL consumer links even when it only names one Ludus:: target (PRIVATE link
# deps of a static library are still part of the consumer's link line). Audited
# from conanfile.py + the module CMakeLists (GraphicsRhi->volk, Text->
# FreeType/HarfBuzz, assertion/concurrency paths->Threads).
if(NOT APPLE)
    ludus_sdk_register_dependency(volk "1.4.357.0" "MIT" bundled)
    if(NOT EMSCRIPTEN)
        ludus_sdk_register_dependency(VulkanMemoryAllocator "3.3.0" "MIT" bundled)
    endif()
endif()
if(NOT EMSCRIPTEN)
    ludus_sdk_register_dependency(freetype "2.14.3"   "FTL-or-GPLv2"        bundled)
    ludus_sdk_register_dependency(harfbuzz "14.5.1"   "MIT-modern"          bundled)
endif()
ludus_sdk_register_dependency(yyjson "0.10.0" "MIT" bundled)
ludus_sdk_register_dependency(miniaudio "0.11.23" "MIT-0" bundled)
ludus_sdk_register_dependency(stb_image "2.30" "MIT-or-Unlicense" bundled)
ludus_sdk_register_dependency(MikkTSpace "3e895b49d05ea07e4c2133156cfa94369e19e409" "Zlib" bundled)
ludus_sdk_register_dependency(xxHash "0.8.3" "BSD-2-Clause" bundled)
ludus_sdk_register_dependency(Threads     "system"    "system"             system)

# System prerequisites that cannot reasonably be bundled.
if(APPLE)
    ludus_sdk_register_system_prerequisite("macOS libc / libc++ runtime matching the recorded cxx_runtime_abi")
else()
    ludus_sdk_register_system_prerequisite("libc / libstdc++ runtime matching the recorded cxx_runtime_abi")
endif()
if(NOT EMSCRIPTEN)
    if(NOT APPLE)
        ludus_sdk_register_system_prerequisite("Vulkan loader (libvulkan.so.1) and a Vulkan-capable GPU/driver for RHI runtime")
    endif()
    ludus_sdk_register_system_prerequisite("POSIX threads (pthread)")
    if(LUDUS_PLATFORM_ENABLE_WAYLAND)
        ludus_sdk_register_system_prerequisite("Wayland client development package (wayland-client) for the Wayland Platform backend")
    endif()
    ludus_sdk_register_system_prerequisite("Host shader compiler (pinned Slang) and spirv-val for ludus_compile_shader at build time")
endif()

# Normalize the ON/OFF sanitizer options to JSON integers (0/1) for the manifest
# template; the assertion-policy fields are already 0/1 from EngineBuildFlavor.
foreach(_san ASAN UBSAN TSAN)
    if(LUDUS_ENABLE_${_san})
        set(LUDUS_ENABLE_${_san}_JSON 1)
    else()
        set(LUDUS_ENABLE_${_san}_JSON 0)
    endif()
endforeach()

ludus_sdk_finalize_identity()

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/LudusSdkManifest.json.in"
    "${PROJECT_BINARY_DIR}/cmake/LudusSdkManifest.json"
    @ONLY
    NEWLINE_STYLE UNIX
)

install(
    FILES
        "${PROJECT_BINARY_DIR}/cmake/LudusConfig.cmake"
        "${PROJECT_BINARY_DIR}/cmake/LudusConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${LUDUS_ENGINE_PACKAGE_NAME}"
)

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/${LUDUS_ENGINE_PACKAGE_NAME}/licenses")
install(FILES "${PROJECT_BINARY_DIR}/cmake/LudusSdkManifest.json" DESTINATION "${CMAKE_INSTALL_DATADIR}/${LUDUS_ENGINE_PACKAGE_NAME}")

# Redistributable dependency notices (P02). Bundle the third-party license
# notices alongside the engine LICENSE so a relocated SDK carries its obligations.
install(
    FILES "${PROJECT_SOURCE_DIR}/cmake/sdk/THIRD_PARTY_NOTICES.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${LUDUS_ENGINE_PACKAGE_NAME}/licenses"
)

# Package-local dependency CMake metadata directory. When a build exports the
# bundled dependency configs (volk/freetype/harfbuzz), they are staged under this
# directory so LudusConfig.cmake resolves them without the producer Conan cache.
# The directory is created at install time; the release packaging step populates
# it with the actual dependency *.cmake + static libraries (see
# docs/development/project-sdk-workflow.md). Create it unconditionally so a
# relocated consumer finds a stable, documented search root.
install(
    DIRECTORY
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${LUDUS_ENGINE_PACKAGE_NAME}/dependencies"
)

install(FILES cmake/LudusDependencies.cmake cmake/LudusShaders.cmake cmake/LudusRendererShaders.cmake cmake/LudusRenderGraph.cmake DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus")
install(FILES cmake/render_graph/build.py DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/render_graph")
install(FILES scripts/python/ludus_tools/__init__.py scripts/python/ludus_tools/errors.py
    scripts/python/ludus_tools/buildlock.py scripts/python/ludus_tools/render_graph.py
    scripts/python/ludus_tools/render_graph_layout.py
    scripts/python/ludus_tools/render_graph_upscale.py scripts/python/ludus_tools/render_graph_sizes.py
    scripts/python/ludus_tools/render_graph_quality.py
    scripts/python/ludus_tools/render_graph_diagnostics.py
    scripts/python/ludus_tools/render_graph_build.py scripts/python/ludus_tools/render_graph_execution.py
    scripts/python/ludus_tools/render_graph_pipeline.py scripts/python/ludus_tools/render_graph_options.py scripts/python/ludus_tools/render_graph_ray.py
    scripts/python/ludus_tools/render_graph_ray_invocation.py
    scripts/python/ludus_tools/render_graph_metal_ray.py
    scripts/python/ludus_tools/render_graph_compute.py
    scripts/python/ludus_tools/render_graph_forward.py
    scripts/python/ludus_tools/render_graph_buffers.py scripts/python/ludus_tools/render_graph_scene.py
    scripts/python/ludus_tools/render_graph_settings.py scripts/python/ludus_tools/render_graph_templates.py
    scripts/python/ludus_tools/render_graph_template_update.py
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/render_graph/ludus_tools")
foreach(template_directory IN ITEMS default forward_lit restir_di visibility_buffer visibility_secondary_rays tiled_vbuffer_nee
        compute_ray_tracing simple_compute_ray_tracing veach_guibas_mis line_sampling ris_projected_solid_angle
        salesin_jarosz_lines polygonal_light_sampling slice_sampling_luminaire steinberg_plt
        steinberg_physical_optics restir_gi)
    install(DIRECTORY "modules/graphics/renderer/render_graphs/${template_directory}/"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/render_graph/templates/${template_directory}")
endforeach()
# Gameplay project helper + host/shipping main templates (project-live-reload).
if(NOT EMSCRIPTEN)
    install(FILES
        cmake/LudusGame.cmake
        cmake/LudusGameShippingMain.cpp
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus")
    install(FILES apps/game_host/main.cpp
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus" RENAME LudusGameHostMain.cpp)
endif()
install(FILES modules/graphics/rhi/shaders/metal_ray_pipeline_layout.h modules/graphics/renderer/shaders/native_metal_ray.slang modules/graphics/rhi/shaders/raster_bindings.slang modules/graphics/rhi/shaders/runtime_bindings.slang modules/graphics/rhi/shaders/ray_query.slang cmake/shaders/compile_ray.py cmake/shaders/compile_shader.py cmake/shaders/compile_raster.py cmake/shaders/compile_graph_stage.py cmake/shaders/compile_metal_ray.py cmake/shaders/native_worker.py cmake/shaders/cook_texture.py config/shader_toolchain.json config/spirv_cross_toolchain.json DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/shaders")

install(FILES third_party/xxhash/LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/${LUDUS_ENGINE_PACKAGE_NAME}/licenses" RENAME xxhash-LICENSE)

install(FILES cmake/LudusReflection.cmake DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus")
install(FILES tools/reflection/generate.py DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Ludus/reflection")

install(FILES third_party/vma/LICENSE.txt DESTINATION "${CMAKE_INSTALL_DATADIR}/${LUDUS_ENGINE_PACKAGE_NAME}/licenses" RENAME VMA-LICENSE)
