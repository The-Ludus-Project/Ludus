# Offline fitter for the renderer's GGX LTC table (modules/graphics/renderer/data); see docs/modules/foundation/math.md.
option(LUDUS_BUILD_LTC_FITTER "Build the offline GGX LTC table fitter" OFF)
if(LUDUS_BUILD_LTC_FITTER)
    add_executable(ludus_ltc_fitter main.cpp)
    target_link_libraries(
        ludus_ltc_fitter
        PRIVATE
            Ludus::FoundationBase
            Ludus::FoundationContainers
            Ludus::FoundationFilesystem
            Ludus::FoundationMath
            Ludus::FoundationThreading
            Ludus::FoundationTime
            Ludus::GraphicsRenderer
    )
    target_compile_options(ludus_ltc_fitter PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
    ludus_apply_project_defaults(ludus_ltc_fitter)
endif()
