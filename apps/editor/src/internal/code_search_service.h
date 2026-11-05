#pragma once

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

namespace ludus::editor
{
/// One search query (CE07, design section 8). The pattern is interpreted as a
/// regular expression when IsRegex, otherwise literally; whole word wraps it in
/// word boundaries; case follows IsCaseSensitive. The query is value-copied onto
/// the worker thread, so it never shares state with the UI.
struct CodeSearchQuery final
{
    QString Pattern;
    bool IsRegex = false;
    bool IsWholeWord = false;
    bool IsCaseSensitive = false;
};

/// One match within one file, in decoded (UTF-16) coordinates: the zero-based
/// line, the match's [StartColumn, EndColumn) within that line, and the line's
/// text for the preview. Columns and the line are what an editor shows, so
/// opening the result maps directly to CodeEditor::GoToLine.
struct CodeSearchMatch final
{
    foundation::int32 Line = 0;
    foundation::int32 StartColumn = 0;
    foundation::int32 EndColumn = 0;
    /// Absolute editor positions of the match, [StartOffset, EndOffset), so
    /// Replace in Files builds a workspace edit without re-finding the match.
    foundation::int32 StartOffset = 0;
    foundation::int32 EndOffset = 0;
    QString LineText;
};

/// Every match found in one file, grouped for the results tree and for building
/// one CodeEditSet per file in Replace in Files.
struct CodeSearchFileResult final
{
    QString AbsolutePath;
    QList<CodeSearchMatch> Matches;
};

/// Find every match of `query` in `text` (already decoded to UTF-16 with LF line
/// endings), in one global pass, appending to outMatches and returning the number
/// appended. Stops once outMatches reaches `remainingCap`, so the whole search
/// stays bounded by CODE_SEARCH_MAX_MATCHES. A zero-width match advances by one.
/// outCompiledError is set and 0 returned when the regex does not compile; an
/// empty pattern matches nothing. This is the pure core the worker and the tests
/// share, with no file or thread dependency.
[[nodiscard]] foundation::int32 FindMatchesInText(const QString& text,
                                                  const CodeSearchQuery& query,
                                                  foundation::int32 remainingCap,
                                                  QList<CodeSearchMatch>& outMatches,
                                                  QString& outCompiledError);

/// Drives a project-wide search off the UI thread (design section 8). It owns a
/// worker QThread that enumerates the authored roots, skips the build output,
/// .git, binary and oversized files, searches dirty buffers from their passed-in
/// snapshots rather than disk, and streams results back in bounded batches. GUI
/// thread only for its own methods; the worker touches no widget or live host.
class CodeSearchService final : public QObject
{
    Q_OBJECT
public:
    explicit CodeSearchService(QObject* parentOrNull = nullptr);
    ~CodeSearchService() override;
    CodeSearchService(const CodeSearchService&) = delete;
    CodeSearchService& operator=(const CodeSearchService&) = delete;

    /// Start a search of `roots` (absolute authored source roots) for `query`,
    /// skipping any path under one of `excludedPrefixes` (absolute, e.g. the
    /// project's `out/` tree and `.git`). `dirtyBuffers` maps an absolute path to
    /// the current unsaved draft (decoded UTF-16 is produced on the worker), so
    /// open edits are searched instead of the stale bytes on disk. A running
    /// search is cancelled first. Results arrive through BatchReady and Finished.
    void Start(const QStringList& roots,
               const QStringList& excludedPrefixes,
               const CodeSearchQuery& query,
               const QHash<QString, QByteArray>& dirtyBuffers);
    /// Cancel a running search; Finished is still emitted with wasCancelled true.
    void Cancel();
    [[nodiscard]] bool IsRunning() const noexcept;

Q_SIGNALS:
    /// A bounded batch of per-file results (at most CODE_SEARCH_RESULT_BATCH
    /// matches across the batch), delivered on the UI thread.
    void BatchReady(const QList<CodeSearchFileResult>& batch);
    /// The search ended. `wasCancelled` is true when Cancel stopped it;
    /// `wasTruncated` is true when it hit CODE_SEARCH_MAX_MATCHES;
    /// `patternErrorOrEmpty` carries a regex-compile error, else is empty.
    void Finished(bool wasCancelled, bool wasTruncated, const QString& patternErrorOrEmpty);

private:
    void retireWorker();
    QThread* mWorkerOrNull = nullptr;
};
} // namespace ludus::editor
