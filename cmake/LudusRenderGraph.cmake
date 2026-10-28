include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/LudusDependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/LudusRendererShaders.cmake")

# Shared RG1 builder and RG2 lowering, also installed for relocated SDK consumers.
# Dependencies stay within ROOT; generated package/build output must be outside it.
# RUNTIME_PACKAGE also cooks <NAME>.lurgrt for RG3 runtime activation and sets
# <NAME>_RUNTIME_PACKAGE in the caller's scope to its absolute build-tree path.
# RAY_PATH_CAPTURE emits only the header of the diagnostic ray path capture variant;
# capture builds are never cooked into runtime packages. RAY_PATH_CAPTURE_PACKAGE (with
# RAY_PATH_CAPTURE) also cooks the editor-only diagnostic <NAME>.lurgrd and sets
# <NAME>_RAY_PATH_CAPTURE_PACKAGE in the caller's scope; runtime activation refuses it.
function(ludus_compile_render_graph)
    cmake_parse_arguments(PARSE_ARGV 0 GRAPH "RUNTIME_PACKAGE;RAY_PATH_CAPTURE;RAY_PATH_CAPTURE_PACKAGE"
        "TARGET;NAME;MANIFEST;ROOT;CONFIGURATION" "")
    if(GRAPH_UNPARSED_ARGUMENTS OR NOT TARGET "${GRAPH_TARGET}" OR
       NOT GRAPH_NAME MATCHES "^[A-Za-z][A-Za-z0-9_]*$" OR NOT GRAPH_MANIFEST)
        message(FATAL_ERROR "ludus_compile_render_graph requires TARGET NAME MANIFEST; optional ROOT CONFIGURATION")
    endif()
    if(GRAPH_RUNTIME_PACKAGE AND GRAPH_RAY_PATH_CAPTURE)
        message(FATAL_ERROR "ludus_compile_render_graph: RAY_PATH_CAPTURE builds emit a header, never a RUNTIME_PACKAGE")
    endif()
    if(GRAPH_RAY_PATH_CAPTURE_PACKAGE AND NOT GRAPH_RAY_PATH_CAPTURE)
        message(FATAL_ERROR "ludus_compile_render_graph: RAY_PATH_CAPTURE_PACKAGE requires RAY_PATH_CAPTURE")
    endif()
    get_filename_component(manifest "${GRAPH_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT GRAPH_ROOT)
        get_filename_component(GRAPH_ROOT "${manifest}" DIRECTORY)
    endif()
    get_filename_component(root "${GRAPH_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT GRAPH_CONFIGURATION)
        set(GRAPH_CONFIGURATION Default)
    endif()
    ludus_require_file("${manifest}" "Render graph compilation" "Restore the authored build manifest.")
    ludus_require_python("Render graph compilation")
    ludus_require_tool(LUDUS_SLANG_COMPILER "Render graph compilation" "Run ./scripts/shader-probe bootstrap" -version)
    set(driver "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/render_graph/build.py")
    set(package "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/render_graph")
    set(helper "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaders")
    # Every file the helper reads, named as the SDK installs them flat in its shaders directory.
    # The cook depends on each one, so the stage list and DEPENDS cannot drift apart.
    ludus_get_renderer_shader_roots(renderer_directory renderer_includes)
    file(READ "${renderer_directory}/recipes.json" renderer_recipes)
    set(helper_files compile_shader.py compile_raster.py compile_graph_stage.py compile_metal_ray.py native_worker.py
        recipes.json renderer_lit.slang renderer_environment_layout.h renderer_gpu_preview_layout.h native_metal_ray.slang
        renderer_surface_functions.slang scene_shader.slang scene_payload.slang visibility_buffer.slang ray_shader.slang
        ray_triangle_loop.slang ray_bvh_traversal.slang renderer_reference_layout.h scene_data.h scene_shader_layout.h
        lighting_layout.h math.slang raster_bindings.slang metal_ray_pipeline_layout.h
        shader_toolchain.json spirv_cross_toolchain.json)
    foreach(recipe renderer_gpu_preview renderer_gpu_preview_image renderer_authored renderer_authored_shadow renderer_flat renderer_output renderer_shadow)
        string(JSON source GET "${renderer_recipes}" "${recipe}" source)
        list(APPEND helper_files "${source}")
    endforeach()
    list(REMOVE_DUPLICATES helper_files)
    if(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/python/ludus_tools/render_graph_build.py")
        set(package "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/python")
        # Build dependencies read only installed/tool/source files. Writable staging belongs to this target.
        set(helper "${CMAKE_CURRENT_BINARY_DIR}/ludus-graphs/${GRAPH_TARGET}/${GRAPH_NAME}/helper")
        file(MAKE_DIRECTORY "${helper}")
        # The source tree spreads the helper over the renderer include roots; each name has one owner.
        # configure_file reruns CMake when a source changes, and rewrites the copy only if its content does.
        set(helper_roots "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaders" "${renderer_directory}" ${renderer_includes}
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../config")
        foreach(name IN LISTS helper_files)
            set(owners)
            foreach(directory IN LISTS helper_roots)
                if(EXISTS "${directory}/${name}")
                    list(APPEND owners "${directory}/${name}")
                endif()
            endforeach()
            list(LENGTH owners owner_count)
            if(NOT owner_count EQUAL 1)
                message(FATAL_ERROR "Render graph helper ${name} needs exactly one source owner; found: ${owners}")
            endif()
            configure_file("${owners}" "${helper}/${name}" COPYONLY)
        endforeach()
    endif()
    list(TRANSFORM helper_files PREPEND "${helper}/" OUTPUT_VARIABLE helper_dependencies)
    set(tools "${LUDUS_SLANG_COMPILER}")
    set(arguments)
    if(LUDUS_SHADER_WORKER_SOCKET)
        list(APPEND arguments --worker-socket "${LUDUS_SHADER_WORKER_SOCKET}")
        if(TARGET ludus-shader-worker)
            list(APPEND tools ludus-shader-worker)
        elseif(LUDUS_SHADER_WORKER_EXECUTABLE)
            list(APPEND tools "${LUDUS_SHADER_WORKER_EXECUTABLE}")
        endif()
    endif()
    if(LUDUS_SPIRV_VALIDATOR)
        list(APPEND arguments --validator "${LUDUS_SPIRV_VALIDATOR}")
        list(APPEND tools "${LUDUS_SPIRV_VALIDATOR}")
    endif()
    if(LUDUS_SPIRV_CROSS)
        list(APPEND arguments --spirv-cross "${LUDUS_SPIRV_CROSS}")
        list(APPEND tools "${LUDUS_SPIRV_CROSS}" "${LUDUS_SPIRV_CROSS}.build.json")
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/ludus-graphs/${GRAPH_TARGET}/${GRAPH_NAME}")
    cmake_path(IS_PREFIX root "${output}" NORMALIZE is_output_inside_root)
    if(is_output_inside_root)
        message(FATAL_ERROR "Render graph ROOT must be a dedicated source directory outside generated output")
    endif()
    get_property(registered TARGET "${GRAPH_TARGET}" PROPERTY LUDUS_RENDER_GRAPH_NAMES)
    if(GRAPH_NAME IN_LIST registered)
        message(FATAL_ERROR "Duplicate render graph ${GRAPH_NAME} for ${GRAPH_TARGET}")
    endif()
    set_property(TARGET "${GRAPH_TARGET}" APPEND PROPERTY LUDUS_RENDER_GRAPH_NAMES "${GRAPH_NAME}")
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false "${root}/*")
    file(GLOB modules "${package}/ludus_tools/render_graph*.py")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/ludus-graphs/${GRAPH_TARGET}/${GRAPH_NAME}")
    set(outputs "${output}/${GRAPH_NAME}.h")
    if(GRAPH_RAY_PATH_CAPTURE)
        list(APPEND arguments --ray-path-capture)
    endif()
    if(GRAPH_RAY_PATH_CAPTURE_PACKAGE)
        list(APPEND arguments --ray-path-capture-package "${output}/${GRAPH_NAME}.lurgrd")
        list(APPEND outputs "${output}/${GRAPH_NAME}.lurgrd" "${output}/${GRAPH_NAME}.lurgrd.resources.json")
        set(${GRAPH_NAME}_RAY_PATH_CAPTURE_PACKAGE "${output}/${GRAPH_NAME}.lurgrd" PARENT_SCOPE)
    endif()
    if(GRAPH_RUNTIME_PACKAGE)
        list(APPEND arguments --runtime-package "${output}/${GRAPH_NAME}.lurgrt")
        list(APPEND outputs "${output}/${GRAPH_NAME}.lurgrt" "${output}/${GRAPH_NAME}.lurgrt.resources.json")
        set(${GRAPH_NAME}_RUNTIME_PACKAGE "${output}/${GRAPH_NAME}.lurgrt" PARENT_SCOPE)
    endif()
    add_custom_command(OUTPUT ${outputs}
        COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${package}" "${Python3_EXECUTABLE}" "${driver}"
            --manifest "${manifest}" --root "${root}" --output "${output}/package"
            --header "${output}/${GRAPH_NAME}.h" --name "${GRAPH_NAME}" --configuration "${GRAPH_CONFIGURATION}"
            --helper "${helper}" --compiler "${LUDUS_SLANG_COMPILER}" ${arguments}
        DEPENDS ${inputs} ${modules} ${tools} "${driver}"
            "${package}/ludus_tools/buildlock.py" "${package}/ludus_tools/errors.py" ${helper_dependencies}
        VERBATIM)
    target_sources(${GRAPH_TARGET} PRIVATE "${output}/${GRAPH_NAME}.h")
    target_include_directories(${GRAPH_TARGET} PRIVATE "${output}")
endfunction()
