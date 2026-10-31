#pragma once

#include "internal/render_graph_document.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
/// Validated, unpublished recovery data. Digest identifies the exact checkpoint read,
/// so deleting a reviewed checkpoint cannot delete a newer write from another editor.
struct RenderGraphRecoveryCheckpoint final
{
    RenderGraphSnapshot Draft;
    QMap<QString, QByteArray> SavedHashes;
    QByteArray Digest;
    bool IsBuiltIn = false;
};

/// Export recovered raw bytes into a new file only; never overwrites an existing path.
/// Failed writes report the cause and remove only the incomplete newly created copy.
[[nodiscard]] bool WriteRecoveryCopy(const QByteArray& bytes, const QString& path, QString& outError);

/// Local checkpoint files, separate from authored sources and document history. One atomic
/// slot per package/editor session; other sessions are retained until explicit review/discard.
/// No document mutation. GUI thread only. The caller owns durable-directory policy.
class RenderGraphRecoveryStore final
{
public:
    explicit RenderGraphRecoveryStore(const QString& directory);
    /// This editor session's slot for the loaded package; empty for an unloaded document.
    [[nodiscard]] QString GetPath(const RenderGraphDocument& document) const;
    /// Start a new binding slot; existing checkpoints are retained for review.
    void BeginBinding();
    /// Previous bindings' slots for this package, newest first (bounded display inventory).
    [[nodiscard]] QStringList Find(const RenderGraphDocument& document) const;
    /// Atomically checkpoint accepted draft bytes and saved-baseline hashes. Failed writes
    /// preserve the previous checkpoint and every authored source. Reports the repair locally.
    [[nodiscard]] bool Write(const RenderGraphDocument& document, QString& outError);
    /// Bounded checksum/schema/package validation; failure preserves outCheckpoint.
    [[nodiscard]] bool Read(const QString& path,
                            const RenderGraphDocument& document,
                            RenderGraphRecoveryCheckpoint& outCheckpoint,
                            QString& outError) const;
    /// Delete only a non-linked file in this store whose payload still has expectedDigest.
    /// A concurrent replacement or a failed removal preserves that file and reports failure.
    [[nodiscard]] bool Remove(const QString& path, const QByteArray& expectedDigest, QString& outError) const;

private:
    [[nodiscard]] QString getIdentity(const RenderGraphDocument& document) const;
    [[nodiscard]] bool isLocalFile(const QString& path) const;
    QString mDirectory;
    QString mSession;
};
} // namespace ludus::editor
