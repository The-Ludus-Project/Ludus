#include "internal/code_search_service.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_file_limits.h"
#include "internal/code_text.h"

#include <QAtomicInt>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QThread>

// The project-wide search service (CE07, design section 8). Matching is one pure
// pass (FindMatchesInText) shared by the worker and the tests. The worker is a
// per-job QThread mirroring the recovery writer: it owns deep-copied inputs,
// touches no widget or live host, streams bounded batches back through queued
// signals, and stops at CODE_SEARCH_MAX_MATCHES. A literal query is matched with
// QString::indexOf-equivalent regex escaping; QTextDocument::find is never used.
namespace ludus::editor
{
namespace
{
using namespace foundation;

[[nodiscard]] bool BuildSearchPattern(const CodeSearchQuery& query, QRegularExpression& outPattern, QString& outError)
{
    outError.clear();
    if (query.Pattern.isEmpty() == true)
    {
        return false;
    }
    QString source = query.IsRegex == true ? query.Pattern : QRegularExpression::escape(query.Pattern);
    if (query.IsWholeWord == true)
    {
        source = QStringLiteral("\\b(?:%1)\\b").arg(source);
    }
    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
    if (query.IsCaseSensitive == false)
    {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    // UseUnicodePropertiesOption is left off by default (design section 8).
    QRegularExpression pattern(source, options);
    if (pattern.isValid() == false)
    {
        outError = pattern.errorString();
        return false;
    }
    outPattern = pattern;
    return true;
}
} // namespace

int32 FindMatchesInText(const QString& text,
                        const CodeSearchQuery& query,
                        int32 remainingCap,
                        QList<CodeSearchMatch>& outMatches,
                        QString& outCompiledError)
{
    QRegularExpression pattern;
    if (BuildSearchPattern(query, pattern, outCompiledError) == false)
    {
        return 0;
    }
    if (remainingCap <= 0)
    {
        return 0;
    }
    int32 appended = 0;
    // One global pass over the decoded text. Line and column are derived from the
    // running offset by counting the line starts we cross, so there is no
    // per-match scan from the beginning: the whole file is one walk.
    int32 lineNumber = 0;
    int32 lineStart = 0;
    int32 scannedTo = 0; // The offset up to which lineNumber/lineStart are current.
    int32 offset = 0;
    const int32 size = static_cast<int32>(text.size());
    while (offset <= size && appended < remainingCap)
    {
        const QRegularExpressionMatch match = pattern.match(text, offset);
        if (match.hasMatch() == false)
        {
            break;
        }
        const int32 start = static_cast<int32>(match.capturedStart());
        const int32 end = static_cast<int32>(match.capturedEnd());
        // Advance the line bookkeeping up to this match's start without rescanning
        // earlier text: scannedTo only moves forward across the whole file.
        while (scannedTo < start)
        {
            if (text.at(scannedTo) == QLatin1Char('\n'))
            {
                ++lineNumber;
                lineStart = scannedTo + 1;
            }
            ++scannedTo;
        }
        // A match that spans a line break is clipped to its first line, so a
        // result always maps to one editor line.
        int32 lineEnd = text.indexOf(QLatin1Char('\n'), lineStart);
        if (lineEnd < 0)
        {
            lineEnd = size;
        }
        const int32 matchEndOnLine = end < lineEnd ? end : lineEnd;
        outMatches.append(CodeSearchMatch
        {
            .Line = lineNumber,
            .StartColumn = start - lineStart,
            .EndColumn = matchEndOnLine - lineStart,
            .StartOffset = start,
            .EndOffset = matchEndOnLine,
            .LineText = text.mid(lineStart, lineEnd - lineStart),
        });
        ++appended;
        offset = end > start ? end : end + 1;
    }
    return appended;
}

namespace
{
/// The per-job search worker. Owns copied inputs, runs enumeration and matching
/// in run(), and reports back through queued signals. Mirrors CodeRecoveryWriter.
class CodeSearchWorker final : public QThread
{
    Q_OBJECT
public:
    CodeSearchWorker(QStringList roots,
                     QStringList excludedPrefixes,
                     CodeSearchQuery query,
                     QHash<QString, QByteArray> dirtyBuffers,
                     QObject* parentOrNull)
        : QThread(parentOrNull), mRoots(std::move(roots)), mExcludedPrefixes(std::move(excludedPrefixes)),
          mQuery(std::move(query)), mDirtyBuffers(std::move(dirtyBuffers))
    {
    }
    void RequestCancel()
    {
        mCancelled.storeRelaxed(1);
    }

Q_SIGNALS:
    void BatchReady(const QList<CodeSearchFileResult>& batch);
    void Finished(bool wasCancelled, bool wasTruncated, const QString& patternErrorOrEmpty);

protected:
    void run() override
    {
        // Validate the pattern once; an invalid regex finishes immediately with
        // the compile error so the UI can show it inline.
        QString error;
        {
            QList<CodeSearchMatch> probe;
            static_cast<void>(FindMatchesInText(QString{}, mQuery, 1, probe, error));
        }
        if (error.isEmpty() == false)
        {
            Q_EMIT Finished(false, false, error);
            return;
        }

        int32 remaining = CODE_SEARCH_MAX_MATCHES;
        bool wasTruncated = false;
        QList<CodeSearchFileResult> batch;
        int32 batchMatches = 0;
        const auto flush = [&]() {
            if (batch.isEmpty() == false)
            {
                Q_EMIT BatchReady(batch);
                batch.clear();
                batchMatches = 0;
            }
        };

        for (const QString& root : mRoots)
        {
            if (mCancelled.loadRelaxed() != 0 || remaining <= 0)
            {
                break;
            }
            QDirIterator iterator(root,
                                  QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                  QDirIterator::Subdirectories);
            while (iterator.hasNext() == true)
            {
                if (mCancelled.loadRelaxed() != 0 || remaining <= 0)
                {
                    break;
                }
                const QString path = iterator.next();
                if (isExcluded(path) == true)
                {
                    continue;
                }
                QString decoded;
                if (loadSearchable(path, decoded) == false)
                {
                    continue;
                }
                QList<CodeSearchMatch> matches;
                QString ignored;
                const int32 found = FindMatchesInText(decoded, mQuery, remaining, matches, ignored);
                if (found <= 0)
                {
                    continue;
                }
                remaining -= found;
                if (remaining <= 0)
                {
                    wasTruncated = true;
                }
                batch.append(CodeSearchFileResult{ .AbsolutePath = path, .Matches = std::move(matches) });
                batchMatches += found;
                if (batchMatches >= CODE_SEARCH_RESULT_BATCH)
                {
                    flush();
                }
            }
        }
        flush();
        Q_EMIT Finished(mCancelled.loadRelaxed() != 0, wasTruncated, QString{});
    }

private:
    [[nodiscard]] bool isExcluded(const QString& path) const
    {
        for (const QString& prefix : mExcludedPrefixes)
        {
            if (path == prefix || path.startsWith(prefix + QLatin1Char('/')) == true)
            {
                return true;
            }
        }
        return false;
    }

    // Produce the searchable decoded text for a path: the dirty draft when the
    // file is open and modified, else the on-disk bytes, skipping oversized and
    // binary files. Returns false when the file should not be searched.
    [[nodiscard]] bool loadSearchable(const QString& path, QString& outDecoded) const
    {
        const QHash<QString, QByteArray>::const_iterator dirty = mDirtyBuffers.constFind(path);
        QByteArray bytes;
        if (dirty != mDirtyBuffers.constEnd())
        {
            bytes = dirty.value();
        }
        else
        {
            QFileInfo info(path);
            if (static_cast<usize>(info.size()) > CODE_SEARCH_FILE_LIMIT_BYTES)
            {
                return false;
            }
            QFile file(path);
            if (file.open(QIODevice::ReadOnly) == false)
            {
                return false;
            }
            bytes = file.readAll();
        }
        // Skip a file with a NUL in its first probe window: it is treated as binary.
        const int32 probe =
            static_cast<int32>(qMin(static_cast<usize>(bytes.size()), CODE_SEARCH_BINARY_PROBE_BYTES));
        if (bytes.left(probe).contains('\0') == true)
        {
            return false;
        }
        // DecodeCodeText requires valid UTF-8; invalid bytes are skipped rather
        // than searched as mojibake.
        return DecodeCodeText(bytes, outDecoded);
    }

    QStringList mRoots;
    QStringList mExcludedPrefixes;
    CodeSearchQuery mQuery;
    QHash<QString, QByteArray> mDirtyBuffers;
    QAtomicInt mCancelled{0};
};
} // namespace

CodeSearchService::CodeSearchService(QObject* parentOrNull) : QObject(parentOrNull) {}

CodeSearchService::~CodeSearchService()
{
    Cancel();
    retireWorker();
}

void CodeSearchService::retireWorker()
{
    if (mWorkerOrNull != nullptr)
    {
        mWorkerOrNull->wait();
        delete mWorkerOrNull;
        mWorkerOrNull = nullptr;
    }
}

bool CodeSearchService::IsRunning() const noexcept
{
    return mWorkerOrNull != nullptr && mWorkerOrNull->isRunning() == true;
}

void CodeSearchService::Cancel()
{
    if (mWorkerOrNull != nullptr)
    {
        static_cast<CodeSearchWorker*>(mWorkerOrNull)->RequestCancel();
    }
}

void CodeSearchService::Start(const QStringList& roots,
                              const QStringList& excludedPrefixes,
                              const CodeSearchQuery& query,
                              const QHash<QString, QByteArray>& dirtyBuffers)
{
    // Cancel and join any running search before starting a new one, so only one
    // worker is alive at a time (the recovery-writer discipline).
    Cancel();
    retireWorker();
    CodeSearchWorker* worker = new CodeSearchWorker(roots, excludedPrefixes, query, dirtyBuffers, nullptr);
    mWorkerOrNull = worker;
    connect(worker, &CodeSearchWorker::BatchReady, this, &CodeSearchService::BatchReady);
    connect(worker, &CodeSearchWorker::Finished, this, &CodeSearchService::Finished);
    worker->start(QThread::LowPriority);
}
} // namespace ludus::editor

#include "code_search_service.moc"
