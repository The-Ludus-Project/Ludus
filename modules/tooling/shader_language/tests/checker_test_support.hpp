#pragma once

#include "preprocessor_test_support.hpp"

#include <ludus/tooling/shader_language/semantic.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>

#include <string>

namespace ludus::tooling::shader_language::test
{
/// Preprocess, parse and check one root file, keeping every stage alive for inspection.
class CheckedSource final
{
public:
    explicit CheckedSource(std::string text)
    {
        mSnapshot.AddFile("main.lusl", std::move(text));
        mPreprocessStatus = mSnapshot.Run();
        mParseStatus = Parse(mSnapshot.GetRequest(), mSnapshot.GetResult(), mTree);
        mStatus = Check(mSnapshot.GetRequest(), mSnapshot.GetResult(), mTree, mModel);
    }

    [[nodiscard]] Status GetStatus() const
    {
        return mStatus;
    }

    [[nodiscard]] Status GetParseStatus() const
    {
        return mParseStatus;
    }

    [[nodiscard]] const SyntaxTree& GetTree() const
    {
        return mTree;
    }

    [[nodiscard]] const SemanticModel& GetModel() const
    {
        return mModel;
    }

    [[nodiscard]] bool HasDiagnostic(DiagnosticCode code) const
    {
        for (const Diagnostic& diagnostic : mModel.Diagnostics)
        {
            if (diagnostic.Code == code)
            {
                return true;
            }
        }
        return false;
    }

    /// All diagnostics as `code-name: message` lines, for test failure output.
    [[nodiscard]] std::string DescribeDiagnostics() const
    {
        std::string text;
        for (const Diagnostic& diagnostic : mTree.Diagnostics)
        {
            text += "parse ";
            text += GetDiagnosticCodeName(diagnostic.Code);
            text += ": ";
            text += diagnostic.Message.GetView();
            text += '\n';
        }
        for (const Diagnostic& diagnostic : mModel.Diagnostics)
        {
            text += GetDiagnosticCodeName(diagnostic.Code);
            text += " at ";
            text += std::to_string(diagnostic.Location.Line);
            text += ": ";
            text += diagnostic.Message.GetView();
            text += '\n';
        }
        return text;
    }

    /// Spelling of a node's principal token.
    [[nodiscard]] std::string_view Spell(foundation::uint32 node) const
    {
        const foundation::uint32 token = mTree.Nodes[node].Token;
        return token == NO_TOKEN ? std::string_view{}
                                 : GetTokenSpelling(mSnapshot.GetRequest(),
                                                    mSnapshot.GetResult(),
                                                    mSnapshot.GetResult().Tokens[token]);
    }

    /// The first node of `kind` whose principal token spells `spelling`, or NO_NODE.
    [[nodiscard]] foundation::uint32 FindNode(SyntaxKind kind, std::string_view spelling) const
    {
        for (foundation::uint32 i = 0; i < mTree.Nodes.GetSize(); ++i)
        {
            if (mTree.Nodes[i].Kind == kind && Spell(i) == spelling)
            {
                return i;
            }
        }
        return NO_NODE;
    }

    /// The spelled type of a node.
    [[nodiscard]] std::string TypeOf(foundation::uint32 node) const
    {
        const TypeNameText name =
            FormatTypeName(mModel, mModel.NodeTypes[node], mSnapshot.GetRequest(), mSnapshot.GetResult());
        return std::string(name.GetView());
    }

private:
    Snapshot mSnapshot;
    SyntaxTree mTree;
    SemanticModel mModel;
    Status mPreprocessStatus = Status::InvalidRequest;
    Status mParseStatus = Status::InvalidRequest;
    Status mStatus = Status::InvalidRequest;
};
} // namespace ludus::tooling::shader_language::test
