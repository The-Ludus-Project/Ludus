#pragma once

#include <ludus/foundation/base/config.h>

// -----------------------------------------------------------------------------
// Ludus Band 0 compiler abstraction: compiler identity plus the codegen
// attribute and hint macros the engine uses (inlining, cold/noinline, branch
// prediction, debug break, unreachable). Macros only — no code, free to parse.
//
// Pulled in by <ludus/foundation/base/core.h>; include core.h (or this header
// directly) rather than relying on it arriving transitively. This is the single
// home for "how do I ask this compiler to do X?" so the rest of the engine never
// hand-writes __attribute__/__declspec spellings.
//
// See docs/architecture/foundational-headers.md (§8) and ADR 0007.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Compiler identity.
// -----------------------------------------------------------------------------
#if defined(LUDUS_TARGET_COMPILER) || defined(LUDUS_COMPILER_CLANG) || defined(LUDUS_COMPILER_GCC) ||                  \
    defined(LUDUS_COMPILER_MSVC) || defined(LUDUS_COMPILER_MSVC_ABI)
#    error "Ludus compiler detection outputs must not be overridden"
#endif

#define LUDUS_CXX_CLANG 1
#define LUDUS_CXX_GCC 2
#define LUDUS_CXX_MSVC 3

#if defined(__clang__)
#    define LUDUS_TARGET_COMPILER LUDUS_CXX_CLANG
#    define LUDUS_COMPILER_CLANG 1
#elif defined(_MSC_VER)
#    define LUDUS_TARGET_COMPILER LUDUS_CXX_MSVC
#    define LUDUS_COMPILER_MSVC 1
#elif defined(__GNUC__)
#    define LUDUS_TARGET_COMPILER LUDUS_CXX_GCC
#    define LUDUS_COMPILER_GCC 1
#else
#    error "Ludus compiler detection: unsupported compiler frontend"
#endif

// clang-cl is a Clang frontend with Microsoft ABI/extension compatibility.
#if defined(_MSC_VER)
#    define LUDUS_COMPILER_MSVC_ABI 1
#endif

// Query extensions without defining or overriding the compiler's own macros.
// Reference: LLVM Project, "Clang Language Extensions", feature-checking macros.
// These wrappers follow its __has_builtin/__has_attribute/__has_cpp_attribute
// contracts instead of inferring extension availability from frontend identity:
// https://clang.llvm.org/docs/LanguageExtensions.html
#if defined(__has_builtin)
#    define LUDUS_HAS_BUILTIN(name) __has_builtin(name)
#else
#    define LUDUS_HAS_BUILTIN(name) 0
#endif

#if defined(__has_attribute)
#    define LUDUS_HAS_ATTRIBUTE(name) __has_attribute(name)
#else
#    define LUDUS_HAS_ATTRIBUTE(name) 0
#endif

#if defined(__has_cpp_attribute)
#    define LUDUS_HAS_CPP_ATTRIBUTE(name) __has_cpp_attribute(name)
#else
#    define LUDUS_HAS_CPP_ATTRIBUTE(name) 0
#endif

// -----------------------------------------------------------------------------
// Inlining and codegen attributes.
//
//   LUDUS_INLINE   force inline (best effort; strongest available hint).
//   LUDUS_NOINLINE never inline.
//   LUDUS_COLD     rarely executed; keep off the hot path (e.g. failure sinks).
// -----------------------------------------------------------------------------
#if defined(LUDUS_COMPILER_MSVC_ABI)
#    define LUDUS_INLINE __forceinline
#    define LUDUS_NOINLINE __declspec(noinline)
#elif LUDUS_HAS_ATTRIBUTE(always_inline) && LUDUS_HAS_ATTRIBUTE(noinline)
#    define LUDUS_INLINE inline __attribute__((__always_inline__))
#    define LUDUS_NOINLINE __attribute__((noinline))
#else
#    define LUDUS_INLINE inline
#    define LUDUS_NOINLINE
#endif

#if LUDUS_HAS_ATTRIBUTE(cold)
#    define LUDUS_COLD __attribute__((cold))
#else
#    define LUDUS_COLD
#endif

// Microsoft-ABI targets (MSVC, clang-cl and Clang for windows-msvc) ignore the
// standard [[no_unique_address]] to keep MSVC layout compatibility, and Clang
// warns that it is unknown. Their equivalent is [[msvc::no_unique_address]]:
// https://clang.llvm.org/docs/AttributeReference.html#no-unique-address
#if defined(LUDUS_COMPILER_MSVC_ABI)
#    define LUDUS_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#    define LUDUS_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

// -----------------------------------------------------------------------------
// Branch-prediction hints and low-level codegen intrinsics.
//
// LUDUS_LIKELY/LUDUS_UNLIKELY give a named, greppable engine spelling for use in
// C-style conditions and macros where the C++ [[likely]]/[[unlikely]]
// attributes cannot appear. Prefer the standard attributes in ordinary C++
// control flow; use these in the low-level diagnostic/assertion plumbing.
//
// LUDUS_DEBUG_BREAK traps into an attached debugger; LUDUS_UNREACHABLE marks a
// path the compiler may assume is never taken.
// -----------------------------------------------------------------------------
#if LUDUS_HAS_BUILTIN(__builtin_expect)
#    define LUDUS_LIKELY(expression) (__builtin_expect(!!(expression), 1))
#    define LUDUS_UNLIKELY(expression) (__builtin_expect(!!(expression), 0))
#else
#    define LUDUS_LIKELY(expression) (!!(expression))
#    define LUDUS_UNLIKELY(expression) (!!(expression))
#endif

#if LUDUS_HAS_BUILTIN(__builtin_unreachable)
#    define LUDUS_UNREACHABLE() __builtin_unreachable()
#elif defined(LUDUS_COMPILER_MSVC_ABI)
#    define LUDUS_UNREACHABLE() __assume(0)
#else
#    define LUDUS_UNREACHABLE() ((void)0)
#endif

// WebAssembly has no resumable native debugger trap. An explicit break traps.
#if defined(LUDUS_PLATFORM_WEB)
#    define LUDUS_DEBUG_BREAK() __builtin_trap()
#elif LUDUS_HAS_BUILTIN(__builtin_debugtrap)
#    define LUDUS_DEBUG_BREAK() __builtin_debugtrap()
#elif defined(LUDUS_COMPILER_MSVC_ABI)
#    define LUDUS_DEBUG_BREAK() __debugbreak()
#elif LUDUS_HAS_BUILTIN(__builtin_trap)
#    define LUDUS_DEBUG_BREAK() __builtin_trap()
#else
#    define LUDUS_DEBUG_BREAK() ((void)0)
#endif
