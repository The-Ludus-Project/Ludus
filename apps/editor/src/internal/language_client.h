#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/language_profiles.h"

#include <QByteArray>
#if !defined(Q_OS_WASM)
#    include <QFutureWatcher>
#    include <QJsonDocument>
#endif
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QProcess;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Private native stdio LSP 3.17 client for one profiled server (code editor M4).
/// No document/widget/file ownership. GUI-thread calls; process IO is
/// asynchronous and bounded. Each restart drops outstanding requests. Only
/// UTF-16 and incremental text synchronization are admitted. The profile
/// supplies the language identifier, required capabilities, configuration
/// answers and message vocabulary; the client never interprets a language.
class LanguageClient final : public QObject
{
    Q_OBJECT
public:
    explicit LanguageClient(QObject* parentOrNull = nullptr);
    ~LanguageClient() override;
    /// Stop any running server and start `program` for `profile`. The profile
    /// must outlive the connection; the built-in profiles are static.
    void Start(const LanguageServerProfile& profile,
               const QString& program,
               const QString& workspace,
               const QStringList& arguments = {});
    void Stop();
    /// Retain the latest virtual unit while initializing; opens/changes it when ready.
    void SetDocument(const QString& uri, const QString& text);
    /// Returns 0 while unavailable/initializing. At most one assistance call at a time.
    [[nodiscard]] foundation::int32 Request(const QString& method, const QJsonObject& position);
    /// Whether the ready server advertises this operation; false during initialization.
    [[nodiscard]] bool CanRequest(const QString& method) const;
    void Cancel();

Q_SIGNALS:
    void Ready();
    void Replied(foundation::int32 request, const QJsonValue& result);
    void Failed(const QString& message);
    /// Unsupported operation; preserves the ready process and current virtual document.
    void RequestUnavailable(const QString& message);

private:
    void send(const QJsonObject& message);
    void notify(const QString& method, const QJsonObject& parameters);
    void readMessages();
    void receive(const QJsonObject& message);
    void fail(const QString& message);
    void synchronize();
    [[nodiscard]] bool hasRequiredCapabilities(const QJsonObject& capabilities) const;
    const LanguageServerProfile* mProfile = &GetSlangServerProfile();
    QProcess* mProcessOrNull = nullptr;
    QTimer mDeadline;
#if !defined(Q_OS_WASM)
    QFutureWatcher<QJsonDocument> mDecoder;
#endif
    foundation::uint64 mGeneration = 0;
#if !defined(Q_OS_WASM)
    foundation::uint64 mDecodingGeneration = 0;
#endif
    QByteArray mInput;
    QString mUri;
    QString mText;
    QString mSentUri;
    QString mSentText;
    QJsonObject mCapabilities;
    foundation::int32 mNextRequest = 1;
    foundation::int32 mInitializeRequest = 0;
    foundation::int32 mPendingRequest = 0;
    foundation::int32 mVersion = 0;
    foundation::isize mErrorBytes = 0;
    bool mIsReady = false;
#if !defined(Q_OS_WASM)
    bool mIsDecoding = false;
#endif
};
} // namespace ludus::editor
