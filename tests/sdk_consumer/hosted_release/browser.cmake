# Included before GameRelease.cmake in the generated Color Rally project.
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Color Rally requires the browser Release preset")
endif()

set_target_properties(R5Game PROPERTIES OUTPUT_NAME index SUFFIX ".js")
target_compile_options(R5Game PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(R5Game PRIVATE "-sENVIRONMENT=web" "-sALLOW_MEMORY_GROWTH=1")
configure_file(index.html index.html COPYONLY)
