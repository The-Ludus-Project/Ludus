#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/tooling/shader_language/semantic.hpp>

#include "diagnostic_sink.hpp"

namespace ludus::tooling::shader_language::internal
{
/// FunctionBody::Specialization of a body the checked model describes.
inline constexpr foundation::uint32 NO_SPECIALIZATION = 0xffffffffU;

/// A function body that a pass analyzes.
struct FunctionBody final
{
    /// The SemanticModel::Specializations index whose facts describe the body, or
    /// NO_SPECIALIZATION for the checked model's.
    foundation::uint32 Specialization = NO_SPECIALIZATION;
    /// The FunctionDeclaration node.
    foundation::uint32 Node = NO_NODE;
    /// The function symbol; a specialization's own symbol for a specialized body.
    foundation::uint32 Function = NO_SYMBOL;
};

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

/// @brief Report uniformity errors: derivative operations (`Sample`, `ddx`, `ddy`) and calls of
/// functions that perform them in non-uniform control flow, and varying arguments of `uniform`
/// parameters. Specializations are analyzed with their own facts.
/// @pre The model checked and passed the flow rules without errors.
/// @return False when allocation failed.
[[nodiscard]] bool AnalyzeUniformity(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept;

/// @brief Report values of different coordinate spaces that meet: in an operator, an assignment,
/// an argument, a return, or a matrix applied outside its source space.
/// @pre The model checked without errors.
/// @return False when allocation failed.
[[nodiscard]] bool AnalyzeSpaces(const AnalysisInputs& inputs, DiagnosticSink& inoutSink) noexcept;

/// @brief The function a call of `function` with the type arguments at `firstTypeArgument` (in
/// `model.TypeArguments`, NO_SYMBOL for none) reaches: its specialization's for a specialized
/// template, otherwise `function` itself.
[[nodiscard]] foundation::uint32 ResolveSpecialization(const SemanticModel& model,
                                                       foundation::uint32 function,
                                                       foundation::uint32 firstTypeArgument) noexcept;

/// @brief Gather the bodies of functions, methods, extension methods and generic templates, then one
/// body per specialization; a specialized template's own body is skipped.
/// @return False when allocation failed.
[[nodiscard]] bool CollectFunctionBodies(const SyntaxTree& tree,
                                         const SemanticModel& model,
                                         foundation::Array<FunctionBody>& outBodies) noexcept;

/// @brief MakeSpecializedModel for every specialization, in index order.
/// @return False when allocation failed.
[[nodiscard]] bool MakeSpecializedModels(const SemanticModel& model,
                                         foundation::Array<SemanticModel>& outModels) noexcept;

/// @brief Report style findings (naming, bare `bool` operands) as `setting` says.
/// @pre The model checked without errors.
/// @return False when allocation failed.
[[nodiscard]] bool CheckStyle(const AnalysisInputs& inputs, StyleSetting setting, DiagnosticSink& inoutSink) noexcept;
} // namespace ludus::tooling::shader_language::internal
