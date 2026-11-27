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
        set(includes "${root}/modules/foundation/geometry/shaders"
                     "${root}/modules/foundation/geometry/include/ludus/foundation/geometry/detail"
                     "${root}/modules/content/scene_format/include/ludus/content/scene_format"
                     "${root}/modules/content/render_graph_format/include/ludus/content/render_graph_format"
                     "${root}/modules/content/render_graph_format/include/ludus/content/render_graph_format/detail"
                     "${root}/modules/foundation/math/shaders"
                     "${root}/modules/graphics/rhi/shaders"
                     "${root}/modules/graphics/renderer/include/ludus/graphics/renderer"
                     "${root}/modules/graphics/renderer/include/ludus/graphics/renderer/detail")
    endif()
    set(${out_directory} "${directory}" PARENT_SCOPE)
    set(${out_includes} "${includes}" PARENT_SCOPE)
endfunction()

# The "defines" object of a recipe as NAME=VALUE entries for ludus_compile_shader's DEFINES: configuration values of a
# LUSL recipe, which its lowering also writes as Slang macros for the Slang files it includes.
function(_ludus_renderer_recipe_defines recipes name out_defines)
    set(defines)
    string(JSON count ERROR_VARIABLE error LENGTH "${recipes}" "${name}" defines)
    if(NOT error)
        math(EXPR last "${count} - 1")
        if(count GREATER 0)
            foreach(index RANGE ${last})
                string(JSON macro MEMBER "${recipes}" "${name}" defines ${index})
                string(JSON value GET "${recipes}" "${name}" defines "${macro}")
                list(APPEND defines "${macro}=${value}")
            endforeach()
        endif()
    endif()
    set(${out_defines} "${defines}" PARENT_SCOPE)
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
        string(JSON software_rays ERROR_VARIABLE software_error GET "${recipes}" "${name}" software_ray_queries)
        if(NOT software_error AND software_rays)
            list(APPEND stages SOFTWARE_RAY_QUERIES)
        endif()
        string(JSON fragment_storage ERROR_VARIABLE fragment_error GET "${recipes}" "${name}" fragment_storage)
        if(NOT fragment_error AND fragment_storage)
            list(APPEND stages FRAGMENT_STORAGE)
        endif()
        set(recipe_includes ${includes})
        if(source MATCHES "\\.lusl$" AND NOT directory IN_LIST includes)
            # The lowered module compiles from the build tree, so the recipe directory, which holds the
            # Slang files its extern slang blocks name, becomes an include root as it is for Slang recipes.
            list(APPEND recipe_includes "${directory}")
        endif()
        _ludus_renderer_recipe_defines("${recipes}" "${name}" defines)
        if(defines AND NOT source MATCHES "\\.lusl$")
            message(FATAL_ERROR "Renderer shader recipe ${name} has defines but a Slang source (${source}); only LUSL recipes "
                "take configuration values")
        endif()
        if(defines)
            list(APPEND stages DEFINES ${defines})
        endif()
        ludus_compile_shader(TARGET "${RENDERER_TARGET}" NAME "${name}"
            SOURCE "${directory}/${source}" ${stages} INCLUDES ${recipe_includes} DEPENDS "${manifest}")
    endforeach()
endfunction()

# Generate the <NAME>_slots.h slot tables of built-in LUSL recipes for TARGET without compiling
# them. The renderer binds the recipes its callers compile by these constants; both come from the
# same recipe source, include roots and lowering, so they agree.
function(ludus_generate_renderer_shader_slots)
    cmake_parse_arguments(PARSE_ARGV 0 RENDERER "" "TARGET" "SHADERS")
    if(RENDERER_UNPARSED_ARGUMENTS OR NOT TARGET "${RENDERER_TARGET}" OR NOT RENDERER_SHADERS)
        message(FATAL_ERROR "ludus_generate_renderer_shader_slots requires TARGET and SHADERS")
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
        if(NOT source MATCHES "\\.lusl$")
            message(FATAL_ERROR "Renderer shader recipe ${name} is not LUSL (${source}); only LUSL recipes have a slot table")
        endif()
        _ludus_renderer_recipe_defines("${recipes}" "${name}" defines)
        set(slot_defines)
        if(defines)
            set(slot_defines DEFINES ${defines})
        endif()
        ludus_generate_shader_slots(TARGET "${RENDERER_TARGET}" NAME "${name}"
            SOURCE "${directory}/${source}" INCLUDES ${includes} ${slot_defines} DEPENDS "${manifest}")
    endforeach()
endfunction()
