#include <ludus/tooling/shader_language/pipeline.hpp>

#include <ludus/foundation/base/core.h>
#include <ludus/tooling/shader_language/limits.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>

namespace ludus::tooling::shader_language
{
namespace
{
using foundation::usize;

/// Append a stage's diagnostics to the pipeline's, keeping the MAX_DIAGNOSTICS bound.
[[nodiscard]] bool AppendDiagnostics(const foundation::Array<Diagnostic>& diagnostics,
                                     usize droppedCount,
                                     LoweringResult& inoutResult) noexcept
{
    inoutResult.DroppedDiagnosticCount += droppedCount;
    for (const Diagnostic& diagnostic : diagnostics)
    {
        if (inoutResult.Diagnostics.GetSize() >= MAX_DIAGNOSTICS)
        {
            ++inoutResult.DroppedDiagnosticCount;
            continue;
        }
        if (inoutResult.Diagnostics.TryAdd(diagnostic) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace

Status
LowerToSlang(const PreprocessRequest& request, const LoweringOptions& options, LoweringResult& outResult) noexcept
{
    outResult = LoweringResult{};
    PreprocessResult preprocessed;
    Status outcome = Preprocess(request, preprocessed);
    outResult.SourceReadMask = foundation::Move(preprocessed.SourceReadMask);
    outResult.ConfigurationReadMask = foundation::Move(preprocessed.ConfigurationReadMask);
    bool isStored = AppendDiagnostics(preprocessed.Diagnostics, preprocessed.DroppedDiagnosticCount, outResult);
    if (outcome == Status::Ready && isStored == true)
    {
        SyntaxTree tree;
        outcome = Parse(request, preprocessed, tree);
        isStored = AppendDiagnostics(tree.Diagnostics, tree.DroppedDiagnosticCount, outResult);
        if (outcome == Status::Ready && isStored == true)
        {
            SemanticModel model;
            outcome = Check(request, preprocessed, tree, options.Check, model);
            isStored = AppendDiagnostics(model.Diagnostics, model.DroppedDiagnosticCount, outResult);
            if (outcome == Status::Ready && isStored == true)
            {
                EmitResult emitted;
                outcome = EmitSlang(request, preprocessed, tree, model, options.Emit, emitted);
                isStored = AppendDiagnostics(emitted.Diagnostics, emitted.DroppedDiagnosticCount, outResult);
                outResult.Text = foundation::Move(emitted.Text);
                outResult.SourceMap = foundation::Move(emitted.SourceMap);
                outResult.BoundsChecks = foundation::Move(emitted.BoundsChecks);
                outResult.Specializations = foundation::Move(emitted.Specializations);
                outResult.GroupsharedClears = foundation::Move(emitted.GroupsharedClears);
            }
        }
    }
    outResult.Outcome = isStored == true ? outcome : Status::OutOfMemory;
    if (outResult.Outcome != Status::Ready)
    {
        outResult.Text.Clear();
        outResult.SourceMap.Clear();
        outResult.BoundsChecks.Clear();
        outResult.Specializations.Clear();
        outResult.GroupsharedClears.Clear();
    }
    return outResult.Outcome;
}
} // namespace ludus::tooling::shader_language
