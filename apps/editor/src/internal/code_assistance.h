#pragma once

#include "internal/code_document_controller.h"
#include "internal/slang_language_service.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>

namespace ludus::editor
{
/// Private editing controller for semantic assistance. All result publication is
/// guarded by the document identity, context generation, request and caret.
/// The editor/adapter outlive this object. Never saves authored files.
class CodeAssistance final : public QObject
{
    Q_OBJECT
public:
    CodeAssistance(CodeEditor& inoutEditor, CodeDocumentController& inoutDocument, QObject* parentOrNull = nullptr);
    /// Invalidate an incompatible context and all pending replies/popups.
    void ClearContext();
    /// Supply compiler-owned generated surroundings and the server's virtual URI.
    /// Source text is always taken from the current authoritative draft.
    void SetContext(const QString& server,
                    const QString& workspace,
                    const QString& uri,
                    const QString& prefix,
                    const QString& suffix,
                    const QStringList& serverArguments = {});
    void Request(bool isSignature);
    void Cancel();

Q_SIGNALS:
    void ContextRequested();
    void ContextCancelled();
    void StatusChanged(const QString& message);

private:
    struct Completion final
    {
        QString Label;
        QString Detail;
        QJsonObject Item;
    };
    void cancelPending();
    void requestCurrent();
    void receive(foundation::int32 request, const QJsonValue& value);
    void showSignatures();
    void acceptCompletion(foundation::int32 row);
    [[nodiscard]] bool isCurrent() const;
    [[nodiscard]] bool mapPosition(const QJsonObject& position, foundation::usize& outBytes) const;
    CodeEditor& mEditor;
    CodeDocumentController& mDocument;
    SlangLanguageService mService;
    QTimer mRefresh;
    CodeTextCapture mCapture;
    QString mServer;
    QString mServerStamp;
    QStringList mServerArguments;
    QString mUri;
    QString mPrefix;
    QString mSuffix;
    QString mUnit;
    QList<Completion> mCompletions;
    QJsonArray mSignatures;
    foundation::uint64 mContext = 0;
    foundation::uint64 mRequestedContext = 0;
    foundation::int32 mCaret = 0;
    foundation::int32 mRequest = 0;
    foundation::int32 mWordStart = 0;
    foundation::int32 mWordEnd = 0;
    foundation::int32 mActiveSignature = 0;
    foundation::int32 mActiveParameter = 0;
    bool mIsSignature = false;
    bool mHasIntent = false;
    bool mShouldTrackSignature = false;
    bool mShouldTrackCompletion = false;
};
} // namespace ludus::editor
