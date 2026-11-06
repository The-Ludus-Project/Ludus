#pragma once

#include "preprocessor_test_support.hpp"

#include <ludus/tooling/shader_language/syntax.hpp>

#include <string>

namespace ludus::tooling::shader_language::test
{
/// Preprocess and parse one root file, keeping every stage alive for inspection.
class ParsedSource final
{
public:
    explicit ParsedSource(std::string text)
    {
        mSnapshot.AddFile("main.lusl", std::move(text));
        mPreprocessStatus = mSnapshot.Run();
        mStatus = Parse(mSnapshot.GetRequest(), mSnapshot.GetResult(), mTree);
    }

    [[nodiscard]] Status GetStatus() const
    {
        return mStatus;
    }

    [[nodiscard]] Status GetPreprocessStatus() const
    {
        return mPreprocessStatus;
    }

    [[nodiscard]] const SyntaxTree& GetTree() const
    {
        return mTree;
    }

    [[nodiscard]] bool HasDiagnostic(DiagnosticCode code) const
    {
        for (const Diagnostic& diagnostic : mTree.Diagnostics)
        {
            if (diagnostic.Code == code)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string_view Spell(foundation::uint32 token) const
    {
        return GetTokenSpelling(mSnapshot.GetRequest(), mSnapshot.GetResult(), mSnapshot.GetResult().Tokens[token]);
    }

    /// The tree as nested `(kind token children...)` text, for golden comparisons.
    [[nodiscard]] std::string Dump() const
    {
        std::string text;
        if (mTree.Nodes.IsEmpty() == false)
        {
            dumpNode(0, text);
        }
        return text;
    }

    /// The subtree of the first top-level declaration, or the whole tree when there is none.
    [[nodiscard]] std::string DumpFirstDeclaration() const
    {
        std::string text;
        if (mTree.Nodes.IsEmpty() == false && mTree.Nodes[0].FirstChild != NO_NODE)
        {
            dumpNode(mTree.Nodes[0].FirstChild, text);
        }
        return text;
    }

private:
    void dumpNode(foundation::uint32 index, std::string& inoutText) const
    {
        const SyntaxNode& node = mTree.Nodes[index];
        inoutText += '(';
        inoutText += GetSyntaxKindName(node.Kind);
        if (node.IsStatic == true)
        {
            inoutText += " static";
        }
        if (node.IsConst == true)
        {
            inoutText += " const";
        }
        if (node.IsGroupshared == true)
        {
            inoutText += " groupshared";
        }
        if (node.Direction == ParameterDirection::In)
        {
            inoutText += " in";
        }
        else if (node.Direction == ParameterDirection::Out)
        {
            inoutText += " out";
        }
        else if (node.Direction == ParameterDirection::Inout)
        {
            inoutText += " inout";
        }
        if (node.Token != NO_TOKEN)
        {
            inoutText += ' ';
            inoutText += Spell(node.Token);
        }
        for (foundation::uint32 child = node.FirstChild; child != NO_NODE; child = mTree.Nodes[child].NextSibling)
        {
            inoutText += ' ';
            dumpNode(child, inoutText);
        }
        inoutText += ')';
    }

    Snapshot mSnapshot;
    SyntaxTree mTree;
    Status mPreprocessStatus = Status::InvalidRequest;
    Status mStatus = Status::InvalidRequest;
};
} // namespace ludus::tooling::shader_language::test
