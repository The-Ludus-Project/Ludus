#pragma once

// Platform-module configuration.
//
// OS *family* and CPU *architecture* detection (LUDUS_PLATFORM_WINDOWS/MACOS/
// LINUX/DESKTOP, LUDUS_ARCH_*) is a foundational concern and lives in
// <ludus/foundation/base/config.h>; this header re-exposes it so existing
// platform-module includes keep working, and adds only what is genuinely a
// platform-module concern: which windowing backend is active.
// macOS selects LUDUS_PLATFORM_COCOA and Windows LUDUS_PLATFORM_WIN32; their
// AppKit and Win32 types stay private.
//
// The active windowing backend is selected by the build system, not inferred
// here: modules/platform/CMakeLists.txt defines LUDUS_PLATFORM_WAYLAND=1 when
// the Wayland backend is compiled, or LUDUS_PLATFORM_HEADLESS=1 when Wayland is
// unavailable and the null backend is used. Base config.h only classifies the
// OS so that consumers which merely need the OS family do not accidentally
// force a backend on.

#include <ludus/foundation/base/config.h>

#if defined(LUDUS_PLATFORM_BROWSER)
#    if LUDUS_PLATFORM_BROWSER != 1
#        error "Ludus platform configuration: selected backend must equal 1"
#    endif
#endif
#if defined(LUDUS_PLATFORM_WAYLAND)
#    if LUDUS_PLATFORM_WAYLAND != 1
#        error "Ludus platform configuration: selected backend must equal 1"
#    endif
#endif
#if defined(LUDUS_PLATFORM_X11)
#    if LUDUS_PLATFORM_X11 != 1
#        error "Ludus platform configuration: selected backend must equal 1"
#    endif
#endif
#if defined(LUDUS_PLATFORM_HEADLESS)
#    if LUDUS_PLATFORM_HEADLESS != 1
#        error "Ludus platform configuration: selected backend must equal 1"
#    endif
#endif

#if defined(LUDUS_PLATFORM_COCOA)
#    if LUDUS_PLATFORM_COCOA != 1
#        error "Ludus platform configuration: selected backend must equal 1"
#    endif
#    if LUDUS_TARGET_OS != LUDUS_OS_MACOS
#        error "Ludus platform configuration: Cocoa backend requires macOS"
#    endif
#endif

#if defined(LUDUS_PLATFORM_WIN32)
#    if LUDUS_PLATFORM_WIN32 != 1
#        error "Ludus platform configuration: selected backend must equal 1"
#    endif
#    if LUDUS_TARGET_OS != LUDUS_OS_WINDOWS
#        error "Ludus platform configuration: Win32 backend requires Windows"
#    endif
#endif

#if (defined(LUDUS_PLATFORM_BROWSER) + defined(LUDUS_PLATFORM_WAYLAND) + defined(LUDUS_PLATFORM_X11) +                 \
     defined(LUDUS_PLATFORM_HEADLESS) + defined(LUDUS_PLATFORM_COCOA) + defined(LUDUS_PLATFORM_WIN32)) > 1
#    error "Ludus platform configuration: select at most one windowing backend"
#endif

#if defined(LUDUS_PLATFORM_BROWSER) && LUDUS_TARGET_OS != LUDUS_OS_WEB
#    error "Ludus platform configuration: browser backend requires Emscripten"
#endif

#if (defined(LUDUS_PLATFORM_WAYLAND) || defined(LUDUS_PLATFORM_X11)) && LUDUS_TARGET_OS != LUDUS_OS_LINUX
#    error "Ludus platform configuration: Wayland/X11 backend requires desktop Linux"
#endif

#if defined(LUDUS_PLATFORM_HEADLESS) && LUDUS_TARGET_OS == LUDUS_OS_WEB
#    error "Ludus platform configuration: native headless backend cannot target Emscripten"
#endif

#if defined(LUDUS_PLATFORM_WAYLAND)
#    define LUDUS_PLATFORM_HAS_WAYLAND 1
#endif

#if defined(LUDUS_PLATFORM_X11)
#    define LUDUS_PLATFORM_HAS_X11 1
#endif
