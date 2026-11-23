#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "internal/language_positions.h"
#include "internal/language_profiles.h"
#include "internal/language_requests.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#if !defined(Q_OS_WASM)
#    include <QFutureWatcher>
#    include <QJsonDocument>
#endif
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QProcess;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Private native stdio LSP 3.17 client for one profiled server (code editor M4,
/// CE09). No document/widget/file ownership; GUI-thread calls; process IO is
/// asynchronous and bounded. The profile supplies the language identifier,
/// configuration answers and message vocabulary; the client never interprets a
/// language.
///
/// - **Capability dispatch.** A request kind the ready server does not advertise
///   is never sent: Request reports it unavailable, and CanRequest and
///   GetUnavailableReason let callers disable its command first.
/// - **Request table.** At most one request per kind is in flight. A newer
///   request of a kind cancels the older one with `$/cancelRequest`, and the
///   older reply is discarded. Each sent request has its own deadline.
/// - **Priority.** Interactive requests flush pending text changes and go at
///   once. Background and workspace requests wait for the synchronization
///   debounce and then go in priority order.
/// - **Synchronization.** Incremental `didChange` when the server supports it,
///   otherwise full text; both are coalesced to the debounce.
/// - **Positions.** UTF-8 when the server chooses it from the offered list,
///   otherwise UTF-16. Callers map positions with GetPositionEncoding.
/// - **Crashes.** An unexpected exit restarts the same server after a backoff
///   that doubles from one second. A third exit within a minute stops it and
///   reports Failed; Start begins a new history.
class LanguageClient final : public QObject
{
    Q_OBJECT
public:
    explicit LanguageClient(QObject* parentOrNull = nullptr);
    ~LanguageClient() override;
    /// Stop any running server, clear the crash history and start `program` for
    /// `profile`. The profile must outlive the connection; the built-in profiles
    /// are static.
    void Start(const LanguageServerProfile& profile,
               const QString& program,
               const QString& workspace,
               const QStringList& arguments = {});
    void Stop();
    /// Retain the latest virtual unit; opens or changes it at the next debounce or
    /// interactive request once the server is ready.
    void SetDocument(const QString& uri, const QString& text);
    /// Queue or send `kind` with `parameters`; the client adds `textDocument` for
    /// document-scoped kinds. Returns the request identifier, or 0 while the
    /// server is unavailable or initializing, or when it does not advertise `kind`
    /// (then RequestUnavailable is emitted). Cancels an older request of `kind`.
    [[nodiscard]] foundation::int32 Request(LanguageRequestKind kind, const QJsonObject& parameters);
    /// Whether the server is initialized and accepts requests.
    [[nodiscard]] bool IsReady() const noexcept;
    /// Whether the ready server advertises `kind`; false during initialization.
    [[nodiscard]] bool CanRequest(LanguageRequestKind kind) const;
    /// Why `kind` is disabled for the ready server, or empty when it is available
    /// or the server is not ready yet.
    [[nodiscard]] QString GetUnavailableReason(LanguageRequestKind kind) const;
    /// The ready server's `capabilities`; empty before initialization.
    [[nodiscard]] const QJsonObject& GetCapabilities() const noexcept;
    /// The negotiated position encoding; UTF-16 before initialization.
    [[nodiscard]] LanguagePositionEncoding GetPositionEncoding() const noexcept;
    /// The server's retained standard error (at most the diagnostic budget), for
    /// Show Log. Kept after a failure until the next Start.
    [[nodiscard]] const QString& GetLog() const noexcept;
    /// Cancel the queued or in-flight request of `kind`; its reply is discarded.
    void Cancel(LanguageRequestKind kind);
    /// Cancel every queued and in-flight request.
    void Cancel();

Q_SIGNALS:
    void Ready();
    void Replied(foundation::int32 request, LanguageRequestKind kind, const QJsonValue& result);
    /// The connection stopped; editing is unaffected. The message names the fix.
    void Failed(const QString& message);
    /// The server answered `request` with an error; the connection stays up.
    void RequestFailed(foundation::int32 request, LanguageRequestKind kind, const QString& message);
    /// Unsupported operation; preserves the ready process and current virtual document.
    void RequestUnavailable(LanguageRequestKind kind, const QString& message);
    /// The server exited unexpectedly and restarts after a backoff.
    void Restarting(const QString& message);

private:
    struct PendingRequest final
    {
        foundation::int32 Id = 0;
        bool IsSent = false;
        QJsonObject Message;
        QDeadlineTimer Deadline;
    };
    void startProcess();
    void stopProcess();
    void send(const QJsonObject& message);
    void notify(const QString& method, const QJsonObject& parameters);
    void sendRequest(PendingRequest& inoutRequest);
    void flushQueued();
    void armDeadline();
    void readMessages();
    void receive(const QJsonObject& message);
    void receiveInitialize(const QJsonObject& message);
    void handleExit();
    void fail(const QString& message);
    void synchronize();
    [[nodiscard]] PendingRequest& getRequest(LanguageRequestKind kind) noexcept;
    const LanguageServerProfile* mProfile = &GetSlangServerProfile();
    QString mProgram;
    QString mWorkspace;
    QStringList mArguments;
    QProcess* mProcessOrNull = nullptr;
    QTimer mDeadline;
    QTimer mSynchronize;
    QTimer mRestart;
    QDeadlineTimer mInitializeDeadline;
    QElapsedTimer mClock;
    QList<foundation::int64> mCrashTimes;
#if !defined(Q_OS_WASM)
    QFutureWatcher<QJsonDocument> mDecoder;
#endif
    foundation::uint64 mGeneration = 0;
#if !defined(Q_OS_WASM)
    foundation::uint64 mDecodingGeneration = 0;
#endif
    foundation::StaticArray<PendingRequest, LANGUAGE_REQUEST_KIND_COUNT> mRequests{};
    QByteArray mInput;
    QString mLog;
    QString mUri;
    QString mText;
    QString mSentUri;
    QString mSentText;
    QJsonObject mCapabilities;
    LanguagePositionEncoding mEncoding = LanguagePositionEncoding::Utf16;
    foundation::int32 mNextRequest = 1;
    foundation::int32 mInitializeRequest = 0;
    foundation::int32 mVersion = 0;
    foundation::int32 mSyncKind = 0;
    foundation::isize mErrorBytes = 0;
    bool mIsReady = false;
#if !defined(Q_OS_WASM)
    bool mIsDecoding = false;
#endif
};
} // namespace ludus::editor
