function(ludus_configure_project_warnings target_name)
    add_library(${target_name} INTERFACE)

    set(clang_like_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wformat=2
        -Wundef
        -Wnull-dereference
        -Wdouble-promotion
        # Implicit floating-point to integer conversions. MSVC /W4 reports them as C4244, an
        # error under /WX, and the Windows lane runs only in batch validation; this flag lets
        # Linux/macOS pull request builds reject them first. GCC accepts the same spelling.
        -Wfloat-conversion
        -Wimplicit-fallthrough
        -Woverloaded-virtual
        -Wnon-virtual-dtor
    )
    # Clang's reachability warnings. MSVC /W4 reports unreachable code as C4702, an error
    # under /WX, and only batch validation builds with MSVC; these let the Linux pull
    # request build reject the cases Clang can see. MSVC also follows calls it inlines,
    # which Clang does not, so they cannot catch every C4702. Linux only: web and Apple
    # code that MSVC never compiles (an `#if` early return for the browser, say) would
    # otherwise fail only in batch builds, the failure these warnings exist to prevent.
    set(clang_warnings
        -Wunreachable-code
        -Wunreachable-code-loop-increment
    )

    target_compile_options(
        ${target_name}
        INTERFACE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:${clang_like_warnings}>
            $<$<AND:$<CXX_COMPILER_ID:Clang>,$<PLATFORM_ID:Linux>>:${clang_warnings}>
            $<$<CXX_COMPILER_ID:MSVC>:/W4>
            $<$<AND:$<BOOL:${LUDUS_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
            # Emit after the warning groups so Clang does not re-enable it.
            $<$<AND:$<BOOL:$<TARGET_PROPERTY:LUDUS_ALLOW_PARTIAL_INITIALIZERS>>,$<CXX_COMPILER_ID:Clang,AppleClang,GNU>>:-Wno-missing-field-initializers>
            $<$<AND:$<BOOL:${LUDUS_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:Clang,AppleClang,GNU>>:-Werror>
    )
endfunction()
