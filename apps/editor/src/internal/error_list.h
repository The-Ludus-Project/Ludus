#pragma once

// ErrorList: the Error List tool window (private editor header, not installed).
//
// One table of diagnostics from independent producers: the render-graph backend
// (graph validation and mapped shader diagnostics), renderer activation and project build output.
// Each producer replaces only its own rows. The list presents copies; activating a
// row reports the entry so the shell can navigate with the producer's own mapping
// (for render graphs, RendererWorkspace::ActivateDiagnostic). It never edits a
// document or re-runs a job.

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_job.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QTableWidget;
class QPlainTextEdit;
class QResizeEvent;
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
    RenderPreview,
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
    /// Compact position: `line:column` (e.g. `75:12`), a bare line, or a configuration/node path.
    QString Location;
    /// "Render graph", "Shader", "Renderer preview" or "Build".
    QString Origin;
    /// Producer-owned index used for navigation: the RendererWorkspace diagnostic row
    /// for render graphs, or the zero-based Output line for builds. Preview rows open Render Preview.
    foundation::int32 SourceIndex = 0;
    /// Raw tool output that backs the row (a compiler log). It stays collapsed under the row
    /// until the user opens Details; empty when the description already says everything.
    QString Details;
    /// Backends or targets that reported this same diagnostic, in first-seen order, e.g. "metal".
    QStringList Backends;
    /// How many identical diagnostics the row stands for; always at least one.
    foundation::int32 Count = 1;
};

/// Most build diagnostics parsed from one output text.
inline constexpr foundation::int32 MAX_BUILD_DIAGNOSTICS = 1000;

/// Rows for one render-graph job result. Each problem appears once: a shader failure's build
/// summary folds into its first mapped diagnostic as collapsed Details, and the same diagnostic
/// reported by several backends is one row listing them. SourceIndex is the producer's row
/// (RendererWorkspace's diagnostic row order) of the first diagnostic a row stands for.
[[nodiscard]] QList<ErrorListEntry> MakeRenderGraphErrorEntries(const RenderGraphJobResult& result);
/// Current preview failure, independently owned from compiler and project-build results.
/// Empty problem clears the preview producer's rows; file names the authored root graph.
[[nodiscard]] QList<ErrorListEntry> MakeRenderPreviewErrorEntries(const QString& problem, const QString& file);
/// Current preview advisory (RendererWorkspace::GetRenderWarning) as one Warning row of the preview producer,
/// to append to its error rows. Empty warning yields no row; file names the input the warning asks to edit.
[[nodiscard]] QList<ErrorListEntry> MakeRenderPreviewWarningEntries(const QString& warning, const QString& file);
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

    /// Replace every row owned by producer with entries (copied). Emits ProblemsReported when
    /// the new rows include an error or warning and differ from that producer's previous rows.
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
    /// A producer reported a changed set of rows that includes an error or warning. Repeating
    /// an unchanged result does not emit, so a list the user closed stays closed.
    void ProblemsReported();
    /// The rows changed in any way, so counts derived from them may differ.
    void CountsChanged();

protected:
    /// Reserve visible table rows before giving the selected diagnostic remaining height.
    /// The details pane stays collapsed until the user opens Details for a row that has any.
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateDetailsHeight();
    void updateDetailsToggle();
    void render();
    [[nodiscard]] bool isShown(ErrorListSeverity severity) const;

    QList<ErrorListEntry> mEntries;
    QToolButton* mErrors;
    QToolButton* mWarnings;
    QToolButton* mMessages;
    QToolButton* mDetailsToggle;
    QTableWidget* mTable;
    QPlainTextEdit* mDetails;
};
} // namespace ludus::editor
