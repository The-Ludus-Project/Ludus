#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/controller.h"
#include "internal/render_graph_source.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QProcess;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Identity captured when a render-graph job starts; results publish only if it still matches.
struct RenderGraphJobRequest final
{
    foundation::uint64 Request = 0;
    foundation::uint64 ProjectEpoch = 0;
    foundation::uint64 Revision = 0;
    QString Action;
    QByteArray InputIdentity;   ///< Semantic identity of the captured source version and configuration.
    bool ShouldPreview = false; ///< Publish only success matching that source version's current identity.
    RenderGraphSource Source =
        RenderGraphSource::Draft; ///< Automatic previews capture Saved; explicit builds use Draft.
    QStringList Arguments;
    QString SnapshotDirectory;
};

/// Copied CLI result. Payload holds the versioned success object; failures keep the
/// stable error code, message and the authored-source location/diagnostics.
struct RenderGraphJobResult final
{
    RenderGraphJobRequest Request;
    bool IsSuccessful = false;
    bool IsCancelled = false;
    QJsonObject Payload;
    QString Code;
    QString Message;
    QJsonObject Location;
    QJsonArray Diagnostics;
};

// Runs `python -m ludus_tools --json render-graph ...` from the tooling checkout, so
// the editor and CLI share one backend and produce identical results for the same
// draft snapshot. One job at a time; bounded output; Cancel kills the process.
// The GUI thread never waits for it. Results carry the captured request identity.
class RenderGraphJob final : public QObject
{
    Q_OBJECT
public:
    static constexpr qsizetype MAX_OUTPUT_BYTES = qsizetype{2} * 1024 * 1024;

    explicit RenderGraphJob(QObject* parentOrNull = nullptr);
    ~RenderGraphJob() override;
    RenderGraphJob(const RenderGraphJob&) = delete;
    RenderGraphJob& operator=(const RenderGraphJob&) = delete;
    /// Start one job; false when another job is active or the tooling root is missing.
    [[nodiscard]] bool Start(const ToolingPaths& tooling, const RenderGraphJobRequest& request);
    void Cancel();
    [[nodiscard]] bool IsActive() const noexcept;
    /// Parse one CLI JSON document into a result; shared with tests for CLI equivalence.
    [[nodiscard]] static RenderGraphJobResult
    ParseOutput(const RenderGraphJobRequest& request, const QByteArray& output, foundation::int32 exitCode);

Q_SIGNALS:
    void Finished(const ludus::editor::RenderGraphJobResult& result);

private:
    void finish(foundation::int32 exitCode, bool isCrashed);
    QProcess* mProcessOrNull = nullptr;
    RenderGraphJobRequest mRequest;
    QByteArray mOutput;
    bool mIsCancelled = false;
};
} // namespace ludus::editor
