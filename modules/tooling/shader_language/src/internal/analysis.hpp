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

/// @brief Report the flow errors of one specialization, analyzed with `inputs.Model` from
/// MakeSpecializedModel.
/// @return False when allocation failed.
[[nodiscard]] bool AnalyzeSpecializationFlow(const AnalysisInputs& inputs,
                                             foundation::uint32 functionNode,
                                             DiagnosticSink& inoutSink) noexcept;

/// @brief Copy `model` with the facts of specialization `index` in place of its template's facts,
/// so passes that read per-node facts see one specialization.
/// @return False when allocation failed; `outModel` is then incomplete.
[[nodiscard]] bool
MakeSpecializedModel(const SemanticModel& model, foundation::uint32 index, SemanticModel& outModel) noexcept;

/// @brief Report style findings (naming, bare `bool` operands) as `setting` says.
/// @pre The model checked without errors.
/// @return False when allocation failed.
[[nodiscard]] bool CheckStyle(const AnalysisInputs& inputs, StyleSetting setting, DiagnosticSink& inoutSink) noexcept;
} // namespace ludus::tooling::shader_language::internal
