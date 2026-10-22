#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/controller.h"
#include "internal/render_graph_job.h"

#include <QObject>
#include <QString>

namespace ludus::editor
{
/// Creates a complete editable graph package through the shared backend.
/// Owns the asynchronous creation request, independently of an open draft.
class RenderGraphCreation final : public QObject
{
    Q_OBJECT
public:
    explicit RenderGraphCreation(QObject* parentOrNull = nullptr);
    /// Publish under project/graphs/name. Rejects existing destinations and
    /// escaping paths; failure preserves existing files and the open document.
    /// templateIdentity selects raster or restir-di; sources use explicit native host profiles.
    [[nodiscard]] bool Start(const ToolingPaths& tooling,
                             const QString& projectRoot,
                             const QString& name,
                             foundation::uint64 epoch,
                             QString& outError,
                             const QString& templateIdentity = QStringLiteral("raster"));
    /// Cancel and invalidate the pending callback when the project changes.
    void Cancel();
    [[nodiscard]] bool IsActive() const noexcept;

Q_SIGNALS:
    void Finished();
    void Created(const QString& rootGraph, foundation::uint64 epoch);
    void Failed(const QString& message);

private:
    RenderGraphJob mJob;
    foundation::uint64 mRequest = 0;
    QString mRootGraph;
};
} // namespace ludus::editor
