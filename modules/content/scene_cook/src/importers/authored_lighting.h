#pragma once

#include "image/image_reader.h"
#include "import_diagnostics.h"
#include "scene_ir.h"

#include <ludus/content/content.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::content::scene_cook::importers
{
inline constexpr usize LIGHTING_RGB_COMPONENTS = 3;
struct CookedLighting final
{
    Array<scene_format::LightRecord> Lights;
    foundation::StaticArray<float32, LIGHTING_RGB_COMPONENTS> EnvironmentRgb{};
    DecodedLightingImage Environment;
    Digest EnvironmentSourceDigest;
    bool HasEnvironmentSourceDigest = false;
};
struct LightingCookDescription final
{
    float64 RangeMetres = 100;
    usize EnvironmentByteBudget = scene_format::MAX_BUNDLE_BYTES;
    bool ShouldDecodeEnvironment = true;
};
// Validate an explicit source-root-relative latitude/longitude environment selection.
// An authored environment or escaping/missing path rejects without changing the scene.
[[nodiscard]] Status ApplyCompanionEnvironment(std::string_view file,
                                               std::string_view root,
                                               SceneIr& inoutScene,
                                               DiagnosticLog& inoutLog) noexcept;
// Preserve authored energy in the frozen Rec709 photometric profile. Scene data
// and images remain caller-owned; failures preserve outLighting.
[[nodiscard]] Status CookAuthoredLighting(const SceneIr& scene,
                                          std::string_view root,
                                          const LightingCookDescription& description,
                                          CookedLighting& outLighting,
                                          DiagnosticLog& inoutLog) noexcept;
} // namespace ludus::content::scene_cook::importers
