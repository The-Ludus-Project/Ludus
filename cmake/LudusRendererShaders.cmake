include_guard(GLOBAL)

# Built-in renderer recipes have one owner, shared by engine and installed SDK.
# Callers choose artifacts; source paths, entry points and includes stay here.
# Recipe directory (with recipes.json) and include roots of the built-in recipes.
# Run-time recompilers (the editor's live viewport shaders) capture these same roots.
function(ludus_get_renderer_shader_roots out_directory out_includes)
    set(directory "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaders")
    set(includes "${directory}")
    if(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../modules/graphics/renderer/shaders/recipes.json")
        get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
        set(directory "${root}/modules/graphics/renderer/shaders")
        set(includes "${root}/modules/content/scene_format/include/ludus/content/scene_format"
                     "${root}/modules/content/render_graph_format/include/ludus/content/render_graph_format"
                     "${root}/modules/foundation/math/shaders"
                     "${root}/modules/graphics/rhi/shaders"
                     "${root}/modules/graphics/renderer/include/ludus/graphics/renderer"
                     "${root}/modules/graphics/renderer/include/ludus/graphics/renderer/detail")
    endif()
    set(${out_directory} "${directory}" PARENT_SCOPE)
    set(${out_includes} "${includes}" PARENT_SCOPE)
endfunction()

function(ludus_compile_renderer_shaders)
    cmake_parse_arguments(PARSE_ARGV 0 RENDERER "" "TARGET" "SHADERS")
    if(RENDERER_UNPARSED_ARGUMENTS OR NOT TARGET "${RENDERER_TARGET}" OR NOT RENDERER_SHADERS)
        message(FATAL_ERROR "ludus_compile_renderer_shaders requires TARGET and SHADERS")
    endif()
    ludus_get_renderer_shader_roots(directory includes)
    set(manifest "${directory}/recipes.json")
    ludus_require_file("${manifest}" "Renderer shader recipes" "Restore the renderer source/SDK installation.")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
    file(READ "${manifest}" recipes)
    foreach(name IN LISTS RENDERER_SHADERS)
        string(JSON source ERROR_VARIABLE error GET "${recipes}" "${name}" source)
        if(error)
            message(FATAL_ERROR "Unknown renderer shader recipe: ${name}")
        endif()
        get_property(registered TARGET "${RENDERER_TARGET}" PROPERTY LUDUS_RENDERER_SHADER_RECIPES)
        if(name IN_LIST registered)
            message(FATAL_ERROR "Duplicate renderer shader recipe ${name} for ${RENDERER_TARGET}")
        endif()
        set_property(TARGET "${RENDERER_TARGET}" APPEND PROPERTY LUDUS_RENDERER_SHADER_RECIPES "${name}")
        string(JSON compute ERROR_VARIABLE error GET "${recipes}" "${name}" compute)
        if(NOT error)
            set(stages COMPUTE "${compute}")
        else()
            string(JSON vertex GET "${recipes}" "${name}" vertex)
            string(JSON fragment GET "${recipes}" "${name}" fragment)
            set(stages RASTER VERTEX "${vertex}" FRAGMENT "${fragment}")
        endif()
        string(JSON native_rays ERROR_VARIABLE native_error GET "${recipes}" "${name}" native_ray_queries)
        if(NOT native_error AND native_rays)
            list(APPEND stages NATIVE_RAY_QUERIES)
        endif()
        string(JSON fragment_storage ERROR_VARIABLE fragment_error GET "${recipes}" "${name}" fragment_storage)
        if(NOT fragment_error AND fragment_storage)
            list(APPEND stages FRAGMENT_STORAGE)
        endif()
        ludus_compile_shader(TARGET "${RENDERER_TARGET}" NAME "${name}"
            SOURCE "${directory}/${source}" ${stages} INCLUDES ${includes} DEPENDS "${manifest}")
    endforeach()
endfunction()
