// The function bodies that the flow-insensitive passes (uniformity, coordinate spaces) analyze,
// each with the model whose facts describe it.

#include "internal/analysis.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::Array;
using foundation::uint32;

[[nodiscard]] bool HasBlock(const SyntaxTree& tree, uint32 node) noexcept
{
    for (uint32 child = tree.Nodes[node].FirstChild; child != NO_NODE; child = tree.Nodes[child].NextSibling)
    {
        if (tree.Nodes[child].Kind == SyntaxKind::Block)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool AddBody(const SyntaxTree& tree,
                           uint32 specialization,
                           uint32 node,
                           uint32 function,
                           Array<FunctionBody>& inoutBodies) noexcept
{
    if (function == NO_SYMBOL || HasBlock(tree, node) == false)
    {
        return true;
    }
    return inoutBodies.TryAdd(FunctionBody{specialization, node, function});
}

[[nodiscard]] bool IsSpecializedTemplate(const SyntaxTree& tree, const SemanticModel& model, uint32 node) noexcept
{
    for (uint32 child = tree.Nodes[node].FirstChild; child != NO_NODE; child = tree.Nodes[child].NextSibling)
    {
        if (tree.Nodes[child].Kind == SyntaxKind::FunctionDeclaration)
        {
            const uint32 symbol = model.NodeSymbols[child];
            return symbol != NO_SYMBOL && model.Symbols[symbol].IsSpecialized == true;
        }
    }
    return false;
}
} // namespace

bool CollectFunctionBodies(const SyntaxTree& tree, const SemanticModel& model, Array<FunctionBody>& outBodies) noexcept
{
    outBodies.Clear();
    for (uint32 node = tree.Nodes[0].FirstChild; node != NO_NODE; node = tree.Nodes[node].NextSibling)
    {
        const SyntaxKind kind = tree.Nodes[node].Kind;
        if (kind == SyntaxKind::FunctionDeclaration)
        {
            if (AddBody(tree, NO_SPECIALIZATION, node, model.NodeSymbols[node], outBodies) == false)
            {
                return false;
            }
            continue;
        }
        const bool hasMembers = kind == SyntaxKind::TemplateDeclaration || kind == SyntaxKind::StructDeclaration ||
                                kind == SyntaxKind::ExtensionDeclaration;
        // A specialized template is analyzed once per specialization, whose facts it lacks.
        if (hasMembers == false ||
            (kind == SyntaxKind::TemplateDeclaration && IsSpecializedTemplate(tree, model, node) == true))
        {
            continue;
        }
        for (uint32 member = tree.Nodes[node].FirstChild; member != NO_NODE; member = tree.Nodes[member].NextSibling)
        {
            if (tree.Nodes[member].Kind == SyntaxKind::FunctionDeclaration &&
                AddBody(tree, NO_SPECIALIZATION, member, model.NodeSymbols[member], outBodies) == false)
            {
                return false;
            }
        }
    }
    for (uint32 index = 0; index < model.Specializations.GetSize(); ++index)
    {
        const Specialization& specialization = model.Specializations[index];
        if (AddBody(tree, index, specialization.FirstNode, specialization.Function, outBodies) == false)
        {
            return false;
        }
    }
    return true;
}

bool MakeSpecializedModels(const SemanticModel& model, Array<SemanticModel>& outModels) noexcept
{
    outModels.Clear();
    if (outModels.TryEnsureCapacity(model.Specializations.GetSize()) == false)
    {
        return false;
    }
    for (uint32 index = 0; index < model.Specializations.GetSize(); ++index)
    {
        SemanticModel specialized;
        if (MakeSpecializedModel(model, index, specialized) == false)
        {
            return false;
        }
        outModels.Add(foundation::Move(specialized));
    }
    return true;
}
} // namespace ludus::tooling::shader_language::internal
