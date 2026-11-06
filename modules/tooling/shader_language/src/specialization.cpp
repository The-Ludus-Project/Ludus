// Views of one template specialization (docs/modules/tooling/shader-language.md#concepts-and-specialization):
// the model with the specialization's per-node facts in place of its template's, for passes that
// read per-node facts.

#include "internal/analysis.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::Array;
using foundation::uint32;
using foundation::usize;

template <typename Element>
[[nodiscard]] bool CopyArray(const Array<Element>& source, Array<Element>& outTarget) noexcept
{
    outTarget.Clear();
    if (outTarget.TryEnsureCapacity(source.GetSize()) == false)
    {
        return false;
    }
    for (const Element& element : source)
    {
        outTarget.Add(element);
    }
    return true;
}

/// Writes the specialization's saved facts over its template's node range.
template <typename Element>
void Overlay(const Array<Element>& facts, const Specialization& specialization, Array<Element>& inoutTarget) noexcept
{
    for (uint32 i = 0; i < specialization.NodeCount; ++i)
    {
        inoutTarget[specialization.FirstNode + i] = facts[specialization.FirstFact + i];
    }
}
} // namespace

bool MakeSpecializedModel(const SemanticModel& model, uint32 index, SemanticModel& outModel) noexcept
{
    outModel = SemanticModel{};
    outModel.Outcome = model.Outcome;
    if (CopyArray(model.Types, outModel.Types) == false || CopyArray(model.Symbols, outModel.Symbols) == false ||
        CopyArray(model.Members, outModel.Members) == false ||
        CopyArray(model.NodeTypes, outModel.NodeTypes) == false ||
        CopyArray(model.NodeSymbols, outModel.NodeSymbols) == false ||
        CopyArray(model.NodeParameters, outModel.NodeParameters) == false ||
        CopyArray(model.NodeCallKinds, outModel.NodeCallKinds) == false ||
        CopyArray(model.NodeIntegerConstants, outModel.NodeIntegerConstants) == false ||
        CopyArray(model.NodeTypeArguments, outModel.NodeTypeArguments) == false ||
        CopyArray(model.TypeArguments, outModel.TypeArguments) == false ||
        CopyArray(model.TypeParameters, outModel.TypeParameters) == false ||
        CopyArray(model.Conformances, outModel.Conformances) == false ||
        CopyArray(model.Specializations, outModel.Specializations) == false)
    {
        return false;
    }
    if (index >= model.Specializations.GetSize())
    {
        return true;
    }
    const Specialization& specialization = model.Specializations[index];
    const uint32 first = specialization.FirstNode;
    const uint32 count = specialization.NodeCount;
    const usize end = static_cast<usize>(specialization.FirstFact) + count;
    if (static_cast<usize>(first) + count > model.NodeTypes.GetSize() || end > model.SpecializedNodeTypes.GetSize())
    {
        return true;
    }
    Overlay(model.SpecializedNodeTypes, specialization, outModel.NodeTypes);
    Overlay(model.SpecializedNodeSymbols, specialization, outModel.NodeSymbols);
    Overlay(model.SpecializedNodeParameters, specialization, outModel.NodeParameters);
    Overlay(model.SpecializedNodeCallKinds, specialization, outModel.NodeCallKinds);
    Overlay(model.SpecializedNodeIntegerConstants, specialization, outModel.NodeIntegerConstants);
    Overlay(model.SpecializedNodeTypeArguments, specialization, outModel.NodeTypeArguments);
    return true;
}
} // namespace ludus::tooling::shader_language::internal
