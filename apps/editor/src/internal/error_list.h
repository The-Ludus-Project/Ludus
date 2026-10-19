#pragma once

// ErrorList: the Error List tool window (private editor header, not installed).
//
// One table of diagnostics from independent producers: the render-graph backend
// (graph validation and mapped shader diagnostics) and project build output.
// Each producer replaces only its own rows. The list presents copies; activating a
// row reports the entry so the shell can navigate with the producer's own mapping
// (for render graphs, RendererWorkspace::ActivateDiagnostic). It never edits a
// document or re-runs a job.

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_job.h"

#include <QList>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QTableWidget;
class QToolButton;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Severity filter class of one row.
enum class ErrorListSeverity : foundation::uint8
{
    Error,
    Warning,
    Message,
};

/// Producer that owns a row; ReplaceEntries replaces one producer's rows.
enum class ErrorListProducer : foundation::uint8
{
    RenderGraph,
    Build,
};

/// One copied diagnostic.
struct ErrorListEntry final
{
    ErrorListSeverity Severity = ErrorListSeverity::Message;
    ErrorListProducer Producer = ErrorListProducer::RenderGraph;
    /// Stable error code (RenderGraph*, Shader*), or empty.
    QString Code;
    QString Description;
    /// File as the producer reports it: package-relative for render graphs, as printed for builds.
    QString File;
    /// Human-readable position: a line number, or a configuration/node path.
    QString Location;
    /// "Render graph", "Shader" or "Build".
    QString Origin;
    /// Producer-owned index used for navigation: the RendererWorkspace diagnostic row
    /// for render graphs, or the zero-based Output line for builds.
    foundation::int32 SourceIndex = 0;
};

/// Most build diagnostics parsed from one output text.
inline constexpr foundation::int32 MAX_BUILD_DIAGNOSTICS = 1000;

/// Rows for one render-graph job result, in RendererWorkspace's diagnostic row order:
/// the result summary first, then each mapped diagnostic. SourceIndex is that row.
[[nodiscard]] QList<ErrorListEntry> MakeRenderGraphErrorEntries(const RenderGraphJobResult& result);
/// Compiler (`file:line[:column]: error|warning|note: text`) and CMake
/// (`CMake Error|Warning at file:line`) diagnostics in build output, at most
/// MAX_BUILD_DIAGNOSTICS. SourceIndex is the zero-based output line.
[[nodiscard]] QList<ErrorListEntry> ParseBuildErrorEntries(const QString& output);

/// Error List tool window with Visual Studio-style severity filters. UI-thread only.
class ErrorList final : public QWidget
{
    Q_OBJECT
public:
    explicit ErrorList(QWidget* parentOrNull = nullptr);

    /// Replace every row owned by producer with entries (copied).
    void ReplaceEntries(ErrorListProducer producer, const QList<ErrorListEntry>& entries);
    /// All rows, filtered or not, in display order.
    [[nodiscard]] const QList<ErrorListEntry>& GetEntries() const noexcept
    {
        return mEntries;
    }
    [[nodiscard]] foundation::int32 GetCount(ErrorListSeverity severity) const;
    /// Number of rows the severity filters currently show.
    [[nodiscard]] foundation::int32 GetVisibleCount() const;
    /// Activate the n-th shown row, exactly like a double-click. False when out of range.
    bool ActivateVisibleRow(foundation::int32 row);

Q_SIGNALS:
    /// The user activated a row; the shell navigates to its source.
    void EntryActivated(const ludus::editor::ErrorListEntry& entry);

private:
    void render();
    [[nodiscard]] bool isShown(ErrorListSeverity severity) const;

    QList<ErrorListEntry> mEntries;
    QToolButton* mErrors;
    QToolButton* mWarnings;
    QToolButton* mMessages;
    QTableWidget* mTable;
};
} // namespace ludus::editor
