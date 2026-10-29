#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/graphics/rhi/raster.h>

#include <QByteArray>
#include <QList>

namespace ludus::editor
{
/// Built-in renderer recipe drawn by the editor viewport. Each value names one entry of the
/// renderer manifest (modules/graphics/renderer/shaders/recipes.json); see GetPreviewShaderRecipeName.
enum class PreviewShaderProgram : foundation::uint8
{
    Flat,                         ///< renderer_flat: renderer initialization.
    Composite,                    ///< renderer_composite: view composition.
    Material,                     ///< renderer_material: Solid and Albedo materials.
    Output,                       ///< renderer_output: lighting output.
    Shadow,                       ///< renderer_shadow: shadow atlas casters.
    Lit,                          ///< renderer_lit: Lit materials.
    LitShadow,                    ///< renderer_lit_shadow: Lit shadow casters.
    LitDebug,                     ///< renderer_lit_debug: debug views of unlit Worlds.
    LitDebugShadow,               ///< renderer_lit_debug_shadow: their shadow casters.
    ClusteredAuthored,            ///< renderer_clustered_authored: Lit cooked scenes.
    ClusteredAuthoredShadow,      ///< renderer_clustered_authored_shadow: their shadow casters.
    ClusteredAuthoredDebug,       ///< renderer_clustered_authored_debug: debug views of cooked scenes.
    ClusteredAuthoredDebugShadow, ///< renderer_clustered_authored_debug_shadow: their shadow casters.
    Grid,                         ///< renderer_grid: analytic world grid.
};
/// Number of PreviewShaderProgram values.
inline constexpr foundation::usize PREVIEW_SHADER_PROGRAM_COUNT = 14;
/// Raster stage of one PreviewShaderProgram.
enum class PreviewShaderStage : foundation::uint8
{
    Vertex,   ///< Vertex entry of the recipe.
    Fragment, ///< Fragment entry of the recipe.
};
/// Number of PreviewShaderStage values.
inline constexpr foundation::usize PREVIEW_SHADER_STAGE_COUNT = 2;
/// Number of program stages a PreviewShaderSet can replace.
inline constexpr foundation::usize PREVIEW_SHADER_SLOT_COUNT =
    PREVIEW_SHADER_PROGRAM_COUNT * PREVIEW_SHADER_STAGE_COUNT;

/// Renderer manifest recipe name of program, for example "renderer_lit".
[[nodiscard]] const char* GetPreviewShaderRecipeName(PreviewShaderProgram program) noexcept;
/// Description compiled into the editor for program and stage. Builds without
/// LUDUS_EDITOR_GPU_PREVIEW carry no renderer shaders and return an empty description.
[[nodiscard]] graphics::rhi::RasterShaderDescription GetBuiltInPreviewShader(PreviewShaderProgram program,
                                                                             PreviewShaderStage stage) noexcept;

/// Replacement target code for one program stage, compiled at run time from edited sources.
/// Exactly one of Spirv (Vulkan) and Msl (Metal) is non-empty.
struct PreviewShaderArtifact final
{
    QList<foundation::uint32> Spirv; ///< SPIR-V words.
    QByteArray Msl;                  ///< Metal Shading Language source text.
    QByteArray Entry;                ///< Entry name inside the artifact.
};

/// Immutable-by-copy set of the viewport's renderer shaders: the built-in artifacts with optional
/// run-time replacements. Replacements change only the target code; bindings, uniform members and
/// vertex inputs always come from the built-in description, so a replacement must keep that
/// interface (the live shader builder verifies this before publishing a set).
///
/// Copies share replacement bytes. A PreviewGpuGeneration keeps its own copy for its whole
/// lifetime, so descriptions returned by Get stay valid while the generation's GPU work runs.
class PreviewShaderSet final
{
public:
    /// Built-in description of program and stage with this set's replacement code, if any.
    /// The returned views borrow this set; keep the set alive and unmodified while they are used.
    [[nodiscard]] graphics::rhi::RasterShaderDescription Get(PreviewShaderProgram program,
                                                             PreviewShaderStage stage) const noexcept;
    /// Replace the target code of program and stage. An artifact without code clears the replacement.
    void SetArtifact(PreviewShaderProgram program, PreviewShaderStage stage, const PreviewShaderArtifact& artifact);
    /// Whether program and stage draw with replacement code.
    [[nodiscard]] bool HasArtifact(PreviewShaderProgram program, PreviewShaderStage stage) const noexcept;
    /// Whether any program stage draws with replacement code.
    [[nodiscard]] bool HasArtifacts() const noexcept;
    /// SHA-256 over every replacement; equal digests draw identical code. Empty sets share one digest.
    [[nodiscard]] QByteArray ComputeDigest() const;
    /// Caller-assigned identity of this set; 0 is the built-in set.
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetRevision() const noexcept
    {
        return mRevision;
    }
    /// Assign the identity reported back when a generation built from this set first presents.
    LUDUS_INLINE void SetRevision(foundation::uint64 revision) noexcept
    {
        mRevision = revision;
    }

private:
    foundation::StaticArray<PreviewShaderArtifact, PREVIEW_SHADER_SLOT_COUNT> mArtifacts{};
    foundation::uint64 mRevision = 0;
};
} // namespace ludus::editor
