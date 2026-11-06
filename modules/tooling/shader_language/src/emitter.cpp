#include <ludus/tooling/shader_language/emitter.hpp>

#include <ludus/foundation/base/core.h>

#include "internal/analysis.hpp"
#include "internal/diagnostic_sink.hpp"
#include "internal/ir.hpp"

namespace ludus::tooling::shader_language
{
namespace
{
using foundation::usize;

[[nodiscard]] Status GetOutcome(const internal::DiagnosticSink& sink) noexcept
{
    if (sink.HasOutOfMemory() == true)
    {
        return Status::OutOfMemory;
    }
    return sink.HasErrors() == true ? Status::Failed : Status::Ready;
}

} // namespace

Status EmitSlang(const PreprocessRequest& request,
                 const PreprocessResult& preprocessed,
                 const SyntaxTree& tree,
                 const SemanticModel& model,
                 const EmitOptions& options,
                 EmitResult& outResult) noexcept
{
    outResult = EmitResult{};
    internal::DiagnosticSink sink(outResult.Diagnostics, outResult.DroppedDiagnosticCount);
    if (preprocessed.Outcome != Status::Ready || tree.Outcome != Status::Ready || model.Outcome != Status::Ready ||
        model.NodeTypes.GetSize() != tree.Nodes.GetSize() || tree.Nodes.IsEmpty() == true)
    {
        sink.ReportError(DiagnosticCode::InvalidRequest,
                         SourceLocation{},
                         SourceLocation{},
                         internal::ComposeMessage("EmitSlang needs Ready preprocessing, parsing and checking results"));
        outResult.Outcome = Status::InvalidRequest;
        return outResult.Outcome;
    }
    const internal::AnalysisInputs inputs{request, preprocessed, tree, model};
    internal::IrModule module;
    internal::LowerModule(inputs, options, sink, module);
    if (sink.HasErrors() == false && sink.HasOutOfMemory() == false)
    {
        internal::EmitModule(inputs, module, sink, outResult);
    }
    outResult.Outcome = GetOutcome(sink);
    if (outResult.Outcome != Status::Ready)
    {
        outResult.Text.Clear();
        outResult.SourceMap.Clear();
        outResult.BoundsChecks.Clear();
    }
    return outResult.Outcome;
}

} // namespace ludus::tooling::shader_language
