# luslc: command-line checking, lowering and formatting of LUSL sources over Ludus::ShaderLanguage
# (docs/modules/tooling/shader-language.md#luslc).
add_executable(luslc main.cpp language_server.cpp)
target_link_libraries(luslc PRIVATE Ludus::ShaderLanguage Ludus::FoundationFilesystem Ludus::FoundationStrings
    Ludus::FoundationParsingJson)
ludus_apply_project_defaults(luslc)
install(TARGETS luslc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME luslc_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_luslc.py"
        --luslc "$<TARGET_FILE:luslc>" --repository "${PROJECT_SOURCE_DIR}")
    set_tests_properties(luslc_tests PROPERTIES LABELS "unit;shader;tooling" TIMEOUT 60)
endif()
