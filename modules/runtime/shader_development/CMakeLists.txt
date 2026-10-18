# File delivery is native and opt-in by linking the target; it never links Slang.
if(NOT EMSCRIPTEN AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    add_library(ludus_shader_development STATIC src/reload.cpp)
    add_library(Ludus::ShaderDevelopment ALIAS ludus_shader_development)
    set_target_properties(ludus_shader_development PROPERTIES EXPORT_NAME ShaderDevelopment)
    target_sources(ludus_shader_development PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
        FILES include/ludus/runtime/shader_development/reload.hpp)
    target_link_libraries(ludus_shader_development PUBLIC Ludus::GraphicsRenderer
        PRIVATE Ludus::FoundationFilesystem)
    ludus_apply_project_defaults(ludus_shader_development)
    install(TARGETS ludus_shader_development EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()
