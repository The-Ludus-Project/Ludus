#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/tooling/shader_compiler/compiler.hpp>

#include "internal/preview_shader_set.h"

#include <QAtomicInt>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

/// @file
/// Background rebuild of the editor viewport's renderer shaders from edited engine sources.
/// Everything here runs on a worker thread except where a function says otherwise; nothing
/// touches a device, widget or RenderPreview.
namespace ludus::editor
{
/// Where the built-in renderer recipes and their includes live in an engine checkout.
struct PreviewShaderSources final
{
    /// Directory holding recipes.json and the recipe sources.
    QString RecipeDirectory;
    /// Include roots captured with RecipeDirectory, in the build's order.
    QStringList IncludeDirectories;
};
/// Source paths and entries of one recipe, read from the renderer manifest.
struct PreviewShaderRecipe final
{
    QString Source;           ///< Recipe source file name, relative to RecipeDirectory.
    QByteArray VertexEntry;   ///< Vertex entry point.
    QByteArray FragmentEntry; ///< Fragment entry point.
};
/// Every recipe the viewport draws with, indexed by PreviewShaderProgram.
using PreviewShaderRecipes = foundation::StaticArray<PreviewShaderRecipe, PREVIEW_SHADER_PROGRAM_COUNT>;
/// Read the viewport's recipes from RecipeDirectory/recipes.json. Failure preserves outRecipes.
[[nodiscard]] bool
ReadPreviewShaderRecipes(const PreviewShaderSources& sources, PreviewShaderRecipes& outRecipes, QString& outError);

/// Immutable copy of the recipe sources and every file their quoted `#include` directives reach,
/// among the .slang/.h files under the source roots. Names are relative to their root, hidden and
/// out/ directories are skipped, and identical duplicates across roots collapse to one file.
/// Files no recipe includes stay out of the compiler's snapshot, so unrelated renderer helpers
/// do not count against its file and byte limits.
struct PreviewShaderSnapshot final
{
    QStringList Names;       ///< Root-relative names given to the compiler.
    QList<QByteArray> Texts; ///< UTF-8 text of each name.
    QStringList Paths;       ///< Absolute path of each name, for diagnostics.
    /// Absolute path of every .slang/.h file under the roots, captured or not, so watching them
    /// notices an edit that adds an include or creates a missing one.
    QStringList CandidatePaths;
};
/// Read the snapshot of the recipes' sources. An include resolves relative to the including file,
/// then as a root-relative name, as the compiler resolves it; one found in no root is left for
/// the compiler to report. Every preprocessor branch is followed. The captured files are bounded
/// by the compiler's file and byte limits. Failure preserves outSnapshot and explains why in
/// outError.
[[nodiscard]] bool CapturePreviewShaderSnapshot(const PreviewShaderSources& sources,
                                                const PreviewShaderRecipes& recipes,
                                                PreviewShaderSnapshot& outSnapshot,
                                                QString& outError);

/// First error of a failed compilation, for the viewport overlay.
struct PreviewShaderDiagnostic final
{
    QString Path;               ///< Absolute path when the file is in the snapshot, else the compiler's name.
    foundation::int32 Line = 0; ///< One-based line, or 0 when the compiler gave none.
    QString Message;            ///< Error text without location or code.
    QStringList Excerpt;        ///< The first lines the compiler printed after the error.
};
/// Number of compiler output lines kept in PreviewShaderDiagnostic::Excerpt.
inline constexpr qsizetype PREVIEW_SHADER_EXCERPT_LINES = 3;
/// Extract the first error from compiler diagnostics. Without a recognizable location the
/// whole first line becomes Message.
[[nodiscard]] PreviewShaderDiagnostic ParsePreviewShaderDiagnostic(const QByteArray& diagnostics,
                                                                   const PreviewShaderSnapshot& snapshot);
/// "path:line", the message and the excerpt, one per line; usable on any thread.
[[nodiscard]] QString FormatPreviewShaderDiagnostic(const PreviewShaderDiagnostic& diagnostic);

/// Entry name of a SPIR-V module's only entry point. Failure preserves outEntry.
[[nodiscard]] bool FindSpirvEntry(const QList<foundation::uint32>& words, QByteArray& outEntry);
/// Entry name of the only [[vertex]] or [[fragment]] function in Metal source. Failure preserves outEntry.
[[nodiscard]] bool FindMetalEntry(const QByteArray& code, PreviewShaderStage stage, QByteArray& outEntry);
/// Whether a stage compiled from edited sources keeps the interface the editor's pipelines were
/// built for: every resource it uses must be a built-in binding of the same kind (a uniform buffer
/// with the same size, member offsets and shapes), and a vertex stage must read the same inputs.
/// Unused built-in bindings are allowed. Otherwise outReason names the first difference.
/// reflection is the compiler's target JSON with nativeEntryPointResourceUsage.
[[nodiscard]] bool IsPreviewShaderInterfaceCompatible(const QByteArray& reflection,
                                                      PreviewShaderStage stage,
                                                      bool isMetal,
                                                      const graphics::rhi::RasterShaderDescription& builtIn,
                                                      QString& outReason);

/// Synchronous compiler used by PreviewShaderBuilder on its worker thread.
class PreviewShaderCompiler
{
public:
    virtual ~PreviewShaderCompiler() = default;
    /// Compile request into outResult. Returns early with InvalidState once cancel is nonzero.
    [[nodiscard]] virtual tooling::shader_compiler::Status Compile(const tooling::shader_compiler::Request& request,
                                                                   tooling::shader_compiler::Result& outResult,
                                                                   const QAtomicInt& cancel) noexcept = 0;
};
/// Client of a headless ludus-shader-worker listening on a Unix socket. Waits for a worker that
/// is still starting, so the editor can launch one and submit at once.
class PreviewShaderWorkerCompiler final : public PreviewShaderCompiler
{
public:
    explicit PreviewShaderWorkerCompiler(const QString& endpoint);
    [[nodiscard]] tooling::shader_compiler::Status Compile(const tooling::shader_compiler::Request& request,
                                                           tooling::shader_compiler::Result& outResult,
                                                           const QAtomicInt& cancel) noexcept override;

private:
    QByteArray mEndpoint;
};

/// Outcome of one PreviewShaderBuilder::Build.
enum class PreviewShaderBuildStatus : foundation::uint8
{
    Ready,           ///< Shaders holds the edited set; equal to the built-in set when nothing changed.
    CompileFailed,   ///< A stage did not compile or validate; Diagnostic describes the first error.
    RestartRequired, ///< A stage changed its bindings or inputs; Message names the difference.
    Unavailable,     ///< Sources, manifest or compiler are unavailable; Message explains why.
    Cancelled,       ///< The build was cancelled.
};
/// Result of one build. Only Ready carries a usable Shaders set.
struct PreviewShaderBuildResult final
{
    PreviewShaderBuildStatus Status = PreviewShaderBuildStatus::Unavailable;
    PreviewShaderSet Shaders;
    PreviewShaderDiagnostic Diagnostic;
    QString Message;
    /// Stage compilations this build ran; stages whose read files are unchanged are reused.
    foundation::int32 CompiledStages = 0;
    /// Number of shader files watched under the source roots, captured or not.
    foundation::int32 SourceFiles = 0;
    /// Absolute paths of every candidate source file and the manifest, for change notifications.
    QStringList WatchedPaths;
};
/// Rebuild the viewport shaders from a fresh snapshot, recompiling only stages whose read files
/// changed. Owned and called by one worker thread; keeps the last good output of each stage.
class PreviewShaderBuilder final
{
public:
    /// target is Spirv on Vulkan hosts and Metal on macOS. validatorPath, when not empty, is a
    /// spirv-val executable run on every new SPIR-V artifact (Vulkan 1.1 environment).
    PreviewShaderBuilder(PreviewShaderSources sources, tooling::shader_compiler::Target target, QString validatorPath);
    [[nodiscard]] PreviewShaderBuildResult Build(PreviewShaderCompiler& inoutCompiler, const QAtomicInt& cancel);

private:
    struct Stage final
    {
        QString Source;
        QByteArray Entry;
        PreviewShaderStage Kind = PreviewShaderStage::Vertex;
        bool HasOutput = false;
        PreviewShaderArtifact Artifact;
        QByteArray Reflection;
        /// SHA-256 of each file the last successful compilation read, by name.
        QHash<QString, QByteArray> Reads;
    };
    [[nodiscard]] qsizetype findStage(const QString& source, const QByteArray& entry, PreviewShaderStage kind);
    [[nodiscard]] bool needsCompile(const Stage& stage, const QHash<QString, QByteArray>& hashes) const;
    [[nodiscard]] bool compileStage(Stage& inoutStage,
                                    const PreviewShaderSnapshot& snapshot,
                                    PreviewShaderCompiler& inoutCompiler,
                                    const QAtomicInt& cancel,
                                    PreviewShaderBuildResult& inoutResult);
    [[nodiscard]] bool validateSpirv(const QList<foundation::uint32>& words, QString& outError) const;
    PreviewShaderSources mSources;
    tooling::shader_compiler::Target mTarget;
    QString mValidatorPath;
    QList<Stage> mStages;
};
} // namespace ludus::editor
