# Reference GameHost executable (project-live-reload design 2). A project's own
# host target is built the same way via the LudusGame.cmake helper against its
# installed SDK; this in-tree target proves the entry/lifecycle path and backs
# the native acceptance journey. No Qt.

add_executable(ludus_game_host_app main.cpp)
set_target_properties(ludus_game_host_app PROPERTIES OUTPUT_NAME ludus_game_host)
target_link_libraries(ludus_game_host_app PRIVATE
    Ludus::GameHost Ludus::FoundationLogging)
ludus_apply_project_defaults(ludus_game_host_app)

if(LUDUS_BUILD_TESTS)
    # The real publisher validates a symbol-bearing host alongside its module.
    # Assertion-policy Release still runs that full acceptance, so keep the
    # thin reference entry's symbols without altering Release optimization.
    if(MSVC)
        # Windows keeps the host's symbols in a linked PDB, as for the fixture modules.
        set_target_properties(ludus_game_host_app PROPERTIES MSVC_DEBUG_INFORMATION_FORMAT Embedded)
        target_link_options(ludus_game_host_app PRIVATE /DEBUG)
    else()
        target_compile_options(ludus_game_host_app PRIVATE -g)
    endif()
endif()

# Do NOT export the host's whole symbol table. The gameplay module calls back
# only through the passed host service table (design 6), never host-exported
# C++ symbols, and is loaded RTLD_LOCAL — so ENABLE_EXPORTS is unnecessary and
# would leak the host engine ABI into the module's symbol scope.
