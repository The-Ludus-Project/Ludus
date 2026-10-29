#include "internal/preview_shader_set.h"

#include <QCryptographicHash>

#if defined(LUDUS_EDITOR_GPU_PREVIEW)
#    include "renderer_clustered_authored.h"
#    include "renderer_clustered_authored_debug.h"
#    include "renderer_clustered_authored_debug_shadow.h"
#    include "renderer_clustered_authored_shadow.h"
#    include "renderer_composite.h"
#    include "renderer_flat.h"
#    include "renderer_grid.h"
#    include "renderer_lit.h"
#    include "renderer_lit_debug.h"
#    include "renderer_lit_debug_shadow.h"
#    include "renderer_lit_shadow.h"
#    include "renderer_material.h"
#    include "renderer_output.h"
#    include "renderer_shadow.h"
#endif

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;
usize GetSlot(PreviewShaderProgram program, PreviewShaderStage stage) noexcept
{
    return static_cast<usize>(program) * PREVIEW_SHADER_STAGE_COUNT + static_cast<usize>(stage);
}
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
template <typename RecipeVertex, typename RecipeFragment>
rhi::RasterShaderDescription
SelectStage(PreviewShaderStage stage, RecipeVertex vertex, RecipeFragment fragment) noexcept
{
    return stage == PreviewShaderStage::Vertex ? vertex() : fragment();
}
#endif
} // namespace

const char* GetPreviewShaderRecipeName(PreviewShaderProgram program) noexcept
{
    switch (program)
    {
        case PreviewShaderProgram::Flat:
            return "renderer_flat";
        case PreviewShaderProgram::Composite:
            return "renderer_composite";
        case PreviewShaderProgram::Material:
            return "renderer_material";
        case PreviewShaderProgram::Output:
            return "renderer_output";
        case PreviewShaderProgram::Shadow:
            return "renderer_shadow";
        case PreviewShaderProgram::Lit:
            return "renderer_lit";
        case PreviewShaderProgram::LitShadow:
            return "renderer_lit_shadow";
        case PreviewShaderProgram::LitDebug:
            return "renderer_lit_debug";
        case PreviewShaderProgram::LitDebugShadow:
            return "renderer_lit_debug_shadow";
        case PreviewShaderProgram::ClusteredAuthored:
            return "renderer_clustered_authored";
        case PreviewShaderProgram::ClusteredAuthoredShadow:
            return "renderer_clustered_authored_shadow";
        case PreviewShaderProgram::ClusteredAuthoredDebug:
            return "renderer_clustered_authored_debug";
        case PreviewShaderProgram::ClusteredAuthoredDebugShadow:
            return "renderer_clustered_authored_debug_shadow";
        case PreviewShaderProgram::Grid:
            return "renderer_grid";
    }
    return "";
}

rhi::RasterShaderDescription GetBuiltInPreviewShader(PreviewShaderProgram program, PreviewShaderStage stage) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    namespace shaders = ludus::shaders;
    switch (program)
    {
        case PreviewShaderProgram::Flat:
            return SelectStage(stage, shaders::renderer_flat::Vertex, shaders::renderer_flat::Fragment);
        case PreviewShaderProgram::Composite:
            return SelectStage(stage, shaders::renderer_composite::Vertex, shaders::renderer_composite::Fragment);
        case PreviewShaderProgram::Material:
            return SelectStage(stage, shaders::renderer_material::Vertex, shaders::renderer_material::Fragment);
        case PreviewShaderProgram::Output:
            return SelectStage(stage, shaders::renderer_output::Vertex, shaders::renderer_output::Fragment);
        case PreviewShaderProgram::Shadow:
            return SelectStage(stage, shaders::renderer_shadow::Vertex, shaders::renderer_shadow::Fragment);
        case PreviewShaderProgram::Lit:
            return SelectStage(stage, shaders::renderer_lit::Vertex, shaders::renderer_lit::Fragment);
        case PreviewShaderProgram::LitShadow:
            return SelectStage(stage, shaders::renderer_lit_shadow::Vertex, shaders::renderer_lit_shadow::Fragment);
        case PreviewShaderProgram::LitDebug:
            return SelectStage(stage, shaders::renderer_lit_debug::Vertex, shaders::renderer_lit_debug::Fragment);
        case PreviewShaderProgram::LitDebugShadow:
            return SelectStage(stage,
                               shaders::renderer_lit_debug_shadow::Vertex,
                               shaders::renderer_lit_debug_shadow::Fragment);
        case PreviewShaderProgram::ClusteredAuthored:
            return SelectStage(stage,
                               shaders::renderer_clustered_authored::Vertex,
                               shaders::renderer_clustered_authored::Fragment);
        case PreviewShaderProgram::ClusteredAuthoredShadow:
            return SelectStage(stage,
                               shaders::renderer_clustered_authored_shadow::Vertex,
                               shaders::renderer_clustered_authored_shadow::Fragment);
        case PreviewShaderProgram::ClusteredAuthoredDebug:
            return SelectStage(stage,
                               shaders::renderer_clustered_authored_debug::Vertex,
                               shaders::renderer_clustered_authored_debug::Fragment);
        case PreviewShaderProgram::ClusteredAuthoredDebugShadow:
            return SelectStage(stage,
                               shaders::renderer_clustered_authored_debug_shadow::Vertex,
                               shaders::renderer_clustered_authored_debug_shadow::Fragment);
        case PreviewShaderProgram::Grid:
            return SelectStage(stage, shaders::renderer_grid::Vertex, shaders::renderer_grid::Fragment);
    }
#else
    static_cast<void>(program);
    static_cast<void>(stage);
#endif
    return {};
}

rhi::RasterShaderDescription PreviewShaderSet::Get(PreviewShaderProgram program,
                                                   PreviewShaderStage stage) const noexcept
{
    rhi::RasterShaderDescription description = GetBuiltInPreviewShader(program, stage);
    const PreviewShaderArtifact& artifact = mArtifacts[GetSlot(program, stage)];
    if (artifact.Spirv.isEmpty() == false)
    {
        description.Artifact.Spirv = {artifact.Spirv.constData(), static_cast<usize>(artifact.Spirv.size())};
        description.Artifact.SpirvEntry = {artifact.Entry.constData(), static_cast<usize>(artifact.Entry.size())};
    }
    else if (artifact.Msl.isEmpty() == false)
    {
        description.Artifact.Msl = {artifact.Msl.constData(), static_cast<usize>(artifact.Msl.size())};
        description.Artifact.MslEntry = {artifact.Entry.constData(), static_cast<usize>(artifact.Entry.size())};
    }
    return description;
}

void PreviewShaderSet::SetArtifact(PreviewShaderProgram program,
                                   PreviewShaderStage stage,
                                   const PreviewShaderArtifact& artifact)
{
    PreviewShaderArtifact& slot = mArtifacts[GetSlot(program, stage)];
    if ((artifact.Spirv.isEmpty() == true && artifact.Msl.isEmpty() == true) || artifact.Entry.isEmpty() == true)
    {
        slot = {};
        return;
    }
    slot = artifact;
}

bool PreviewShaderSet::HasArtifact(PreviewShaderProgram program, PreviewShaderStage stage) const noexcept
{
    const PreviewShaderArtifact& artifact = mArtifacts[GetSlot(program, stage)];
    return artifact.Spirv.isEmpty() == false || artifact.Msl.isEmpty() == false;
}

bool PreviewShaderSet::HasArtifacts() const noexcept
{
    for (const PreviewShaderArtifact& artifact : mArtifacts)
    {
        if (artifact.Spirv.isEmpty() == false || artifact.Msl.isEmpty() == false)
        {
            return true;
        }
    }
    return false;
}

QByteArray PreviewShaderSet::ComputeDigest() const
{
    QCryptographicHash digest(QCryptographicHash::Sha256);
    for (usize slot = 0; slot < PREVIEW_SHADER_SLOT_COUNT; ++slot)
    {
        const PreviewShaderArtifact& artifact = mArtifacts[slot];
        if (artifact.Spirv.isEmpty() == true && artifact.Msl.isEmpty() == true)
        {
            continue;
        }
        const QByteArray words(reinterpret_cast<const char*>(artifact.Spirv.constData()),
                               artifact.Spirv.size() * static_cast<qsizetype>(sizeof(uint32)));
        // Length-prefix every field so adjacent slots cannot alias.
        for (const QByteArray& field :
             {QByteArray::number(static_cast<quint64>(slot)), words, artifact.Msl, artifact.Entry})
        {
            digest.addData(QByteArray::number(field.size()));
            digest.addData(QByteArrayLiteral(":"));
            digest.addData(field);
        }
    }
    return digest.result();
}
} // namespace ludus::editor
