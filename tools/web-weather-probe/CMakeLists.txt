# Run the GameplayWeather golden corpus in Wasm under Node; it must reproduce the native digest.
add_executable(ludus_web_weather_probe main.cpp)
target_link_libraries(ludus_web_weather_probe PRIVATE ludus_weather_golden_corpus)
ludus_apply_project_defaults(ludus_web_weather_probe)
target_link_options(ludus_web_weather_probe PRIVATE
    -fno-exceptions -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
set_target_properties(ludus_web_weather_probe PROPERTIES OUTPUT_NAME index SUFFIX ".js")
add_test(NAME ludus_web_weather_corpus
    COMMAND "${CMAKE_CROSSCOMPILING_EMULATOR}" "$<TARGET_FILE:ludus_web_weather_probe>")
set_tests_properties(ludus_web_weather_corpus PROPERTIES LABELS "web;weather" TIMEOUT 120)
