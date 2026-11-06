#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/tooling/shader_language/semantic.hpp>

#include "diagnostic_sink.hpp"

namespace ludus::tooling::shader_language::internal
{
/// The finished stages that the flow and style passes read; all must outlive the pass.
struct AnalysisInputs final
{
    const PreprocessRequest& Request;
    const PreprocessResult& Preprocessed;
    const SyntaxTree& Tree;
    const SemanticModel& Model;
};

/// @brief Report flow errors: definite assignment, `out` parameters, missing returns, case
/// endings, `groupshared` initializers and recursion.
/// @pre The model checked without errors.
/// @return False when allocation failed.
[[nodiscard]] bool AnalyzeFlow(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept;

/// @brief Report style findings (naming, bare `bool` operands) as `setting` says.
/// @pre The model checked without errors.
/// @return False when allocation failed.
[[nodiscard]] bool CheckStyle(const AnalysisInputs& inputs, StyleSetting setting, DiagnosticSink& inoutSink) noexcept;
} // namespace ludus::tooling::shader_language::internal
