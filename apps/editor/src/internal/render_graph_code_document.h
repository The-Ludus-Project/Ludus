#pragma once

#include "internal/code_document.h"

#include <QString>

namespace ludus::editor
{
class RenderGraphDocument;

/// Source binding over the package's existing history. No widget or file I/O.
class RenderGraphCodeDocument final : public CodeDocumentHost
{
public:
    explicit RenderGraphCodeDocument(RenderGraphDocument& inoutDocument);
    /// Bind another source/epoch and end typing coalescence; identity never reuses a binding.
    void Bind(const QString& file, foundation::uint64 epoch);
    void ResetBinding();
    [[nodiscard]] CodeDocumentSnapshot GetSnapshot() const noexcept override;
    [[nodiscard]] CodeEditStatus ApplyEdit(const CodeDocumentIdentity& expected,
                                           const CodeTextEdit& edit,
                                           CodeTextSelection before,
                                           CodeTextSelection after,
                                           bool shouldMerge) override;
    void BreakUndoGroup() noexcept override;
    [[nodiscard]] bool CanUndo() const noexcept override;
    [[nodiscard]] bool CanRedo() const noexcept override;

private:
    RenderGraphDocument& mDocument;
    QString mFile;
    foundation::uint64 mEpoch = 0;
    foundation::uint64 mBinding = 0;
};
} // namespace ludus::editor
