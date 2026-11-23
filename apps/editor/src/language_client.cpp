#include "internal/language_client.h"

#include <ludus/foundation/base/parse_number.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#if !defined(Q_OS_WASM)
#    include <QProcess>
#    include <QtConcurrent/QtConcurrentRun>
#endif

// Thanks to Microsoft, Language Server Protocol 3.17: Base Protocol, Cancellation
// Support, Text Document Synchronization, Position Encoding negotiation and the
// Language Features this client dispatches, and to the Slang contributors'
// language server, the first profile of this generic client. Use each compiler's
// semantic results rather than a second parser in the editor.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
// https://github.com/shader-slang/slang-vscode-extension
namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 REQUEST_TIMEOUT_MILLISECONDS = 10000;
#if !defined(Q_OS_WASM)
constexpr isize MAX_PROTOCOL_BYTES = isize{4} * 1024 * 1024;
constexpr isize MAX_HEADER_BYTES = 8192;
constexpr isize MAX_ERROR_BYTES = 8192;
constexpr int32 SHUTDOWN_WAIT_MILLISECONDS = 1000;
#endif
constexpr int32 FULL_SYNC_KIND = 1;
constexpr int32 INCREMENTAL_SYNC_KIND = 2;
constexpr int32 METHOD_NOT_FOUND = -32601;
// Pending text changes and background requests wait this long after the last
// edit, about the syntax service's cadence, so typing never sends per keystroke.
constexpr int32 SYNCHRONIZE_DELAY_MILLISECONDS = 30;
// Restart policy (code editor architecture, "Crash handling").
constexpr int32 RESTART_DELAY_MILLISECONDS = 1000;
constexpr int64 CRASH_WINDOW_MILLISECONDS = 60000;
constexpr qsizetype MAX_CRASHES = 3;

constexpr StaticArray<LanguageRequestPriority, 2> QUEUED_PRIORITIES{
    {LanguageRequestPriority::Background, LanguageRequestPriority::Workspace}};
} // namespace

LanguageClient::LanguageClient(QObject* parentOrNull) : QObject(parentOrNull)
{
#if !defined(Q_OS_WASM)
    connect(&mDecoder, &QFutureWatcher<QJsonDocument>::finished, this, [this]() {
        mIsDecoding = false;
        const QJsonDocument message = mDecoder.result();
        if (mDecodingGeneration == mGeneration)
        {
            if (message.isObject() == false)
            {
                fail(QStringLiteral("Invalid %1 response; retry assistance.").arg(mProfile->ProductName));
                return;
            }
            receive(message.object());
        }
        QTimer::singleShot(0, this, &LanguageClient::readMessages);
    });
#endif
    mClock.start();
    mDeadline.setSingleShot(true);
    connect(&mDeadline, &QTimer::timeout, this, [this]() {
        bool hasExpired = mIsReady == false && mInitializeDeadline.hasExpired() == true;
        for (const PendingRequest& request : mRequests)
        {
            hasExpired = hasExpired || (request.IsSent == true && request.Deadline.hasExpired() == true);
        }
        if (hasExpired == true)
        {
            fail(QStringLiteral("%1 timed out. Retry completion to restart %2.")
                     .arg(mProfile->AssistanceTitle, mProfile->ProductName));
            return;
        }
        armDeadline();
    });
    mSynchronize.setSingleShot(true);
    mSynchronize.setInterval(SYNCHRONIZE_DELAY_MILLISECONDS);
    connect(&mSynchronize, &QTimer::timeout, this, [this]() {
        if (mIsReady == true)
        {
            synchronize();
            flushQueued();
        }
    });
    mRestart.setSingleShot(true);
    connect(&mRestart, &QTimer::timeout, this, &LanguageClient::startProcess);
}

LanguageClient::~LanguageClient()
{
    Stop();
}

void LanguageClient::Stop()
{
    mRestart.stop();
    mCrashTimes.clear();
    stopProcess();
}

void LanguageClient::stopProcess()
{
    ++mGeneration;
    mDeadline.stop();
    mSynchronize.stop();
    mIsReady = false;
    mCapabilities = {};
    mEncoding = LanguagePositionEncoding::Utf16;
    mSyncKind = 0;
    for (PendingRequest& request : mRequests)
    {
        request = {};
    }
    mInitializeRequest = 0;
    mInput.clear();
    mSentUri.clear();
    mSentText.clear();
    mErrorBytes = 0;
#if !defined(Q_OS_WASM)
    if (mProcessOrNull != nullptr)
    {
        QProcess* process = mProcessOrNull;
        mProcessOrNull = nullptr;
        process->disconnect(this);
        process->setParent(QCoreApplication::instance());
        if (process->state() == QProcess::NotRunning)
        {
            process->deleteLater();
            return;
        }
        process->kill();
        // Keep teardown asynchronous too; the parent reaps a stopped child.
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
    }
#endif
}

void LanguageClient::Start(const LanguageServerProfile& profile,
                           const QString& program,
                           const QString& workspace,
                           const QStringList& arguments)
{
    Stop();
    mProfile = &profile;
    mProgram = program;
    mWorkspace = workspace;
    mArguments = arguments;
    mLog.clear();
    startProcess();
}

void LanguageClient::startProcess()
{
    stopProcess();
#if defined(Q_OS_WASM)
    Q_EMIT Failed(QStringLiteral("%1 needs native %2; unavailable in the browser editor.")
                      .arg(mProfile->AssistanceTitle, mProfile->ProductName));
#else
    mProcessOrNull = new QProcess(this);
    QProcess* process = mProcessOrNull;
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, process, [process]() {
        process->kill();
        static_cast<void>(process->waitForFinished(SHUTDOWN_WAIT_MILLISECONDS));
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &LanguageClient::readMessages);
    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        // Retain a bounded diagnostic stream for Show Log, independently of JSON frames.
        process->setReadChannel(QProcess::StandardError);
        const QByteArray chunk = process->read(MAX_ERROR_BYTES - mErrorBytes + 1);
        mErrorBytes += chunk.size();
        process->setReadChannel(QProcess::StandardOutput);
        if (mErrorBytes > MAX_ERROR_BYTES)
        {
            fail(QStringLiteral("%1 exceeded its diagnostic budget. Check the selected tool and retry.")
                     .arg(mProfile->ProductName));
            return;
        }
        mLog += QString::fromUtf8(chunk);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            fail(QStringLiteral("Cannot start %1. %2").arg(mProfile->ServerName, mProfile->StartRepair));
        }
    });
    connect(process, &QProcess::finished, this, &LanguageClient::handleExit);
    const QString workspace = mWorkspace;
    connect(process, &QProcess::started, this, [this, workspace]() {
        mInitializeRequest = ++mNextRequest;
        const QJsonObject completionItem{{QStringLiteral("snippetSupport"), false},
                                         {QStringLiteral("insertReplaceSupport"), true}};
        const QJsonObject parameterInformation{{QStringLiteral("labelOffsetSupport"), true}};
        const QJsonObject signatureInformation{{QStringLiteral("parameterInformation"), parameterInformation}};
        const QJsonObject signatureHelp{{QStringLiteral("signatureInformation"), signatureInformation}};
        // Every standard token type and modifier, decoded through the server's legend.
        QJsonArray tokenTypes;
        for (const char* type :
             {"namespace", "type",     "class",      "enum",   "interface", "struct",   "typeParameter", "parameter",
              "variable",  "property", "enumMember", "event",  "function",  "method",   "macro",         "keyword",
              "modifier",  "comment",  "string",     "number", "regexp",    "operator", "decorator"})
        {
            tokenTypes.append(QLatin1StringView(type));
        }
        const QJsonObject semanticTokens{{QStringLiteral("requests"), QJsonObject{{QStringLiteral("full"), true}}},
                                         {QStringLiteral("tokenTypes"), tokenTypes},
                                         {QStringLiteral("tokenModifiers"),
                                          QJsonArray{QStringLiteral("declaration"),
                                                     QStringLiteral("definition"),
                                                     QStringLiteral("readonly"),
                                                     QStringLiteral("static"),
                                                     QStringLiteral("deprecated"),
                                                     QStringLiteral("defaultLibrary")}},
                                         {QStringLiteral("formats"), QJsonArray{QStringLiteral("relative")}},
                                         {QStringLiteral("overlappingTokenSupport"), false},
                                         {QStringLiteral("multilineTokenSupport"), false}};
        const QJsonObject foldingRange{
            {QStringLiteral("lineFoldingOnly"), true},
            {QStringLiteral("foldingRangeKind"),
             QJsonObject{{QStringLiteral("valueSet"),
                          QJsonArray{QStringLiteral("comment"), QStringLiteral("imports"), QStringLiteral("region")}}}},
            {QStringLiteral("foldingRange"), QJsonObject{{QStringLiteral("collapsedText"), true}}}};
        const QJsonObject textDocument{
            {QStringLiteral("synchronization"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}},
            {QStringLiteral("documentSymbol"),
             QJsonObject{{QStringLiteral("hierarchicalDocumentSymbolSupport"), true}}},
            {QStringLiteral("definition"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
            {QStringLiteral("declaration"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
            {QStringLiteral("typeDefinition"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
            {QStringLiteral("implementation"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
            {QStringLiteral("completion"), QJsonObject{{QStringLiteral("completionItem"), completionItem}}},
            {QStringLiteral("signatureHelp"), signatureHelp},
            {QStringLiteral("hover"),
             QJsonObject{{QStringLiteral("contentFormat"),
                          QJsonArray{QStringLiteral("plaintext"), QStringLiteral("markdown")}}}},
            {QStringLiteral("publishDiagnostics"), QJsonObject{{QStringLiteral("versionSupport"), true}}},
            {QStringLiteral("semanticTokens"), semanticTokens},
            {QStringLiteral("foldingRange"), foldingRange},
            {QStringLiteral("inlayHint"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}}}};
        const QJsonObject capabilities{
            {QStringLiteral("general"),
             QJsonObject{{QStringLiteral("positionEncodings"),
                          QJsonArray{GetLanguagePositionEncodingName(LanguagePositionEncoding::Utf8),
                                     GetLanguagePositionEncodingName(LanguagePositionEncoding::Utf16)}}}},
            {QStringLiteral("workspace"),
             QJsonObject{{QStringLiteral("configuration"), true},
                         {QStringLiteral("inlayHint"), QJsonObject{{QStringLiteral("refreshSupport"), true}}},
                         {QStringLiteral("semanticTokens"), QJsonObject{{QStringLiteral("refreshSupport"), true}}}}},
            {QStringLiteral("textDocument"), textDocument}};
        send({{QStringLiteral("id"), mInitializeRequest},
              {QStringLiteral("method"), QStringLiteral("initialize")},
              {QStringLiteral("params"),
               QJsonObject{{QStringLiteral("processId"), QJsonValue::Null},
                           {QStringLiteral("rootUri"), QUrl::fromLocalFile(workspace).toString()},
                           {QStringLiteral("capabilities"), capabilities}}}});
    });
    mInitializeDeadline.setRemainingTime(REQUEST_TIMEOUT_MILLISECONDS);
    // The initialize deadline covers the start itself, before an id is assigned.
    mDeadline.start(REQUEST_TIMEOUT_MILLISECONDS);
    process->start(mProgram, mArguments);
#endif
}

void LanguageClient::handleExit()
{
    const int64 now = mClock.elapsed();
    while (mCrashTimes.isEmpty() == false && now - mCrashTimes.first() > CRASH_WINDOW_MILLISECONDS)
    {
        mCrashTimes.removeFirst();
    }
    mCrashTimes.append(now);
    if (mCrashTimes.size() >= MAX_CRASHES)
    {
        fail(QStringLiteral("%1 stopped %2 times within a minute, so %3 is off. Choose Retry assistance, or check "
                            "its log, then retry.")
                 .arg(mProfile->ProductName)
                 .arg(mCrashTimes.size())
                 .arg(mProfile->AssistanceNoun));
        return;
    }
    // Back off 1 s, then 2 s, so a server that crashes on a document cannot spin.
    const int32 delay = RESTART_DELAY_MILLISECONDS * (mCrashTimes.size() == 1 ? 1 : 2);
    stopProcess();
    mRestart.start(delay);
    Q_EMIT Restarting(QStringLiteral("%1 stopped. Restarting %2 in %3 s; editing is unaffected.")
                          .arg(mProfile->ProductName, mProfile->AssistanceNoun)
                          .arg(delay / RESTART_DELAY_MILLISECONDS));
}

void LanguageClient::fail(const QString& message)
{
    Stop();
    Q_EMIT Failed(message);
}

void LanguageClient::send(const QJsonObject& message)
{
#if !defined(Q_OS_WASM)
    if (mProcessOrNull == nullptr)
    {
        return;
    }
    QJsonObject envelope = message;
    envelope.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    const QByteArray body = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (body.size() > MAX_PROTOCOL_BYTES || mProcessOrNull->bytesToWrite() > MAX_PROTOCOL_BYTES)
    {
        fail(QStringLiteral("%1 exceeded its message budget. Reduce the %2 and retry.")
                 .arg(mProfile->AssistanceTitle, mProfile->ContextNoun));
        return;
    }
    const QByteArray frame =
        QByteArrayLiteral("Content-Length: ") + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
    static_cast<void>(mProcessOrNull->write(frame));
#else
    static_cast<void>(message);
#endif
}

void LanguageClient::notify(const QString& method, const QJsonObject& parameters)
{
    send({{QStringLiteral("method"), method}, {QStringLiteral("params"), parameters}});
}

LanguageClient::PendingRequest& LanguageClient::getRequest(LanguageRequestKind kind) noexcept
{
    return mRequests[static_cast<usize>(kind)];
}

void LanguageClient::Cancel(LanguageRequestKind kind)
{
    PendingRequest& request = getRequest(kind);
    if (request.IsSent == true)
    {
        notify(QStringLiteral("$/cancelRequest"), {{QStringLiteral("id"), request.Id}});
    }
    request = {};
    armDeadline();
}

void LanguageClient::Cancel()
{
    for (usize index = 0; index < LANGUAGE_REQUEST_KIND_COUNT; ++index)
    {
        Cancel(static_cast<LanguageRequestKind>(index));
    }
}

void LanguageClient::SetDocument(const QString& uri, const QString& text)
{
    mUri = uri;
    mText = text;
    if (mIsReady == true && (mUri != mSentUri || mText != mSentText))
    {
        mSynchronize.start();
    }
}

void LanguageClient::synchronize()
{
    if (mUri != mSentUri)
    {
        if (mSentUri.isEmpty() == false)
        {
            notify(QStringLiteral("textDocument/didClose"),
                   {{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), mSentUri}}}});
        }
        mVersion = 1;
        notify(QStringLiteral("textDocument/didOpen"),
               {{QStringLiteral("textDocument"),
                 QJsonObject{{QStringLiteral("uri"), mUri},
                             {QStringLiteral("languageId"), mProfile->LanguageId},
                             {QStringLiteral("version"), mVersion},
                             {QStringLiteral("text"), mText}}}});
        mSentUri = mUri;
    }
    else if (mText != mSentText && mSyncKind == FULL_SYNC_KIND)
    {
        notify(QStringLiteral("textDocument/didChange"),
               {{QStringLiteral("textDocument"),
                 QJsonObject{{QStringLiteral("uri"), mUri}, {QStringLiteral("version"), ++mVersion}}},
                {QStringLiteral("contentChanges"), QJsonArray{QJsonObject{{QStringLiteral("text"), mText}}}}});
    }
    else if (mText != mSentText)
    {
        int32 prefix = 0;
        while (prefix < mText.size() && prefix < mSentText.size() && mText.at(prefix) == mSentText.at(prefix))
        {
            ++prefix;
        }
        if (prefix > 0 && prefix < mSentText.size() && mSentText.at(prefix).isLowSurrogate() == true)
        {
            --prefix;
        }
        int32 suffix = 0;
        while (suffix < mText.size() - prefix && suffix < mSentText.size() - prefix &&
               mText.at(mText.size() - suffix - 1) == mSentText.at(mSentText.size() - suffix - 1))
        {
            ++suffix;
        }
        if (suffix > 0 && suffix < mText.size() && mText.at(mText.size() - suffix).isLowSurrogate() == true)
        {
            --suffix;
        }
        const QJsonObject range{
            {QStringLiteral("start"), MakeLanguagePosition(mSentText, prefix, mEncoding)},
            {QStringLiteral("end"),
             MakeLanguagePosition(mSentText, static_cast<int32>(mSentText.size()) - suffix, mEncoding)}};
        notify(
            QStringLiteral("textDocument/didChange"),
            {{QStringLiteral("textDocument"),
              QJsonObject{{QStringLiteral("uri"), mUri}, {QStringLiteral("version"), ++mVersion}}},
             {QStringLiteral("contentChanges"),
              QJsonArray{QJsonObject{{QStringLiteral("range"), range},
                                     {QStringLiteral("text"), mText.mid(prefix, mText.size() - prefix - suffix)}}}}});
    }
    mSentText = mText;
}

bool LanguageClient::IsReady() const noexcept
{
    return mIsReady;
}

bool LanguageClient::CanRequest(LanguageRequestKind kind) const
{
    return mIsReady == true && IsLanguageRequestAdvertised(mCapabilities, kind) == true;
}

QString LanguageClient::GetUnavailableReason(LanguageRequestKind kind) const
{
    if (mIsReady == false || IsLanguageRequestAdvertised(mCapabilities, kind) == true)
    {
        return {};
    }
    const LanguageRequestDescriptor& descriptor = DescribeLanguageRequest(kind);
    return QStringLiteral("The selected %1 does not support %2.")
        .arg(mProfile->ServerName, QString(descriptor.Method).section(QLatin1Char('/'), 1));
}

const QJsonObject& LanguageClient::GetCapabilities() const noexcept
{
    return mCapabilities;
}

LanguagePositionEncoding LanguageClient::GetPositionEncoding() const noexcept
{
    return mEncoding;
}

const QString& LanguageClient::GetSentUri() const noexcept
{
    return mSentUri;
}

int32 LanguageClient::GetSentVersion() const noexcept
{
    return mVersion;
}

const QString& LanguageClient::GetSentText() const noexcept
{
    return mSentText;
}

const QString& LanguageClient::GetLog() const noexcept
{
    return mLog;
}

int32 LanguageClient::Request(LanguageRequestKind kind, const QJsonObject& parameters)
{
    Cancel(kind);
    if (mIsReady == false)
    {
        return 0;
    }
    const LanguageRequestDescriptor& descriptor = DescribeLanguageRequest(kind);
    if (CanRequest(kind) == false)
    {
        Q_EMIT RequestUnavailable(kind,
                                  GetUnavailableReason(kind) +
                                      QStringLiteral(" Use Find for text matches; current source and selection were "
                                                     "retained."));
        return 0;
    }
    QJsonObject message = parameters;
    if (descriptor.IsDocumentScoped == true)
    {
        message.insert(QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), mUri}});
    }
    PendingRequest& request = getRequest(kind);
    request.Id = ++mNextRequest;
    request.Message = {{QStringLiteral("id"), request.Id},
                       {QStringLiteral("method"), QString(descriptor.Method)},
                       {QStringLiteral("params"), message}};
    if (descriptor.Priority == LanguageRequestPriority::Interactive)
    {
        // An interactive request sees the latest text: synchronize first. A running
        // debounce still fires afterwards and sends the queued background requests.
        synchronize();
        sendRequest(request);
    }
    else if (mSynchronize.isActive() == false)
    {
        mSynchronize.start();
    }
    return request.Id;
}

void LanguageClient::sendRequest(PendingRequest& inoutRequest)
{
    inoutRequest.IsSent = true;
    inoutRequest.Deadline.setRemainingTime(REQUEST_TIMEOUT_MILLISECONDS);
    // send may fail the connection, which resets the table; copy the message first.
    const QJsonObject message = inoutRequest.Message;
    inoutRequest.Message = {};
    send(message);
    armDeadline();
}

void LanguageClient::flushQueued()
{
    for (const LanguageRequestPriority priority : QUEUED_PRIORITIES)
    {
        for (usize index = 0; index < LANGUAGE_REQUEST_KIND_COUNT; ++index)
        {
            PendingRequest& request = mRequests[index];
            if (request.Id != 0 && request.IsSent == false &&
                DescribeLanguageRequest(static_cast<LanguageRequestKind>(index)).Priority == priority)
            {
                sendRequest(request);
                if (mIsReady == false)
                {
                    return;
                }
            }
        }
    }
}

void LanguageClient::armDeadline()
{
    if (mProcessOrNull == nullptr)
    {
        mDeadline.stop();
        return;
    }
    int64 remaining = -1;
    const auto consider = [&remaining](const QDeadlineTimer& deadline) {
        const int64 value = deadline.remainingTime();
        remaining = remaining < 0 ? value : qMin(remaining, value);
    };
    if (mIsReady == false)
    {
        consider(mInitializeDeadline);
    }
    for (const PendingRequest& request : mRequests)
    {
        if (request.IsSent == true)
        {
            consider(request.Deadline);
        }
    }
    if (remaining < 0)
    {
        mDeadline.stop();
        return;
    }
    mDeadline.start(static_cast<int32>(remaining));
}

void LanguageClient::readMessages()
{
#if !defined(Q_OS_WASM)
    if (mProcessOrNull == nullptr)
    {
        return;
    }
    if (mProcessOrNull->bytesAvailable() + mInput.size() > MAX_PROTOCOL_BYTES)
    {
        fail(QStringLiteral("%1 exceeded its protocol buffer budget; assistance stopped.").arg(mProfile->ProductName));
        return;
    }
    if (mIsDecoding == true)
    {
        return;
    }
    const QByteArray chunk = mProcessOrNull->read(MAX_PROTOCOL_BYTES - mInput.size() + 1);
    mInput += chunk;
    if (mInput.size() > MAX_PROTOCOL_BYTES)
    {
        fail(QStringLiteral("%1 response exceeded its message budget; assistance stopped.").arg(mProfile->ProductName));
        return;
    }
    const isize split = mInput.indexOf(QByteArrayLiteral("\r\n\r\n"));
    if (split < 0)
    {
        if (mInput.size() > MAX_HEADER_BYTES)
        {
            fail(QStringLiteral("Invalid %1 protocol header; retry assistance.").arg(mProfile->ProductName));
        }
        return;
    }
    if (split > MAX_HEADER_BYTES)
    {
        fail(QStringLiteral("%1 protocol header exceeded its budget.").arg(mProfile->ProductName));
        return;
    }
    uint64 length = 0;
    bool hasLength = false;
    int32 lengthHeaders = 0;
    for (const QByteArray& header : mInput.left(split).split('\n'))
    {
        const QByteArray line = header.trimmed();
        if (line.startsWith(QByteArrayLiteral("Content-Length: ")) == true)
        {
            const QByteArray digits = line.mid(QByteArrayLiteral("Content-Length: ").size());
            hasLength =
                ++lengthHeaders == 1 && ParseUint64(digits.constData(), static_cast<usize>(digits.size()), length) ==
                                            NumberParseStatus::Success;
        }
    }
    constexpr isize HEADER_END_BYTES = 4;
    if (hasLength == false || length > static_cast<uint64>(MAX_PROTOCOL_BYTES - split - HEADER_END_BYTES))
    {
        fail(QStringLiteral("Invalid %1 protocol length; retry assistance.").arg(mProfile->ProductName));
        return;
    }
    const isize total = split + HEADER_END_BYTES + static_cast<isize>(length);
    if (mInput.size() < total)
    {
        return;
    }
    const QByteArray body = mInput.mid(split + HEADER_END_BYTES, static_cast<isize>(length));
    mInput.remove(0, total);
    mDecodingGeneration = mGeneration;
    mIsDecoding = true;
    // One message at a time, parsed off the input/paint thread. The watcher owns
    // delivery; a stopped/restarted connection discards the decoded generation.
    mDecoder.setFuture(QtConcurrent::run([body]() { return QJsonDocument::fromJson(body); }));
#endif
}

void LanguageClient::receiveInitialize(const QJsonObject& message)
{
    const QJsonObject capabilities =
        message.value(QStringLiteral("result")).toObject().value(QStringLiteral("capabilities")).toObject();
    LanguagePositionEncoding encoding = LanguagePositionEncoding::Utf16;
    const QJsonValue synchronization = capabilities.value(QStringLiteral("textDocumentSync"));
    const int32 syncKind = synchronization.isObject() == true
                               ? synchronization.toObject().value(QStringLiteral("change")).toInt()
                               : synchronization.toInt();
    if (message.contains(QStringLiteral("error")) == true ||
        ReadLanguagePositionEncoding(capabilities.value(QStringLiteral("positionEncoding")).toString(), encoding) ==
            false ||
        (syncKind != FULL_SYNC_KIND && syncKind != INCREMENTAL_SYNC_KIND))
    {
        fail(QStringLiteral("This %1 does not support UTF-8 or UTF-16 positions with full or incremental text "
                            "synchronization. Use the project's pinned tools.")
                 .arg(mProfile->ServerName));
        return;
    }
    mInitializeRequest = 0;
    mCapabilities = capabilities;
    mEncoding = encoding;
    mSyncKind = syncKind;
    // The crash history survives initialization: a server that crashes on the
    // document right after it starts must still reach the crash limit.
    mIsReady = true;
    armDeadline();
    notify(QStringLiteral("initialized"), {});
    if (mUri.isEmpty() == false)
    {
        synchronize();
    }
    Q_EMIT Ready();
}

void LanguageClient::receive(const QJsonObject& message)
{
    if (message.contains(QStringLiteral("method")) == true)
    {
        if (message.contains(QStringLiteral("id")) == true)
        {
            QJsonValue result = QJsonValue::Null;
            if (message.value(QStringLiteral("method")).toString() == QStringLiteral("workspace/configuration"))
            {
                QJsonArray values;
                for (const QJsonValue& value :
                     message.value(QStringLiteral("params")).toObject().value(QStringLiteral("items")).toArray())
                {
                    values.append(
                        mProfile->ConfigurationResponder(value.toObject().value(QStringLiteral("section")).toString()));
                }
                result = values;
            }
            send({{QStringLiteral("id"), message.value(QStringLiteral("id"))}, {QStringLiteral("result"), result}});
            const QString method = message.value(QStringLiteral("method")).toString();
            if (method == QStringLiteral("workspace/inlayHint/refresh") ||
                method == QStringLiteral("workspace/semanticTokens/refresh"))
            {
                Q_EMIT RefreshRequested();
            }
        }
        else if (message.value(QStringLiteral("method")).toString() ==
                 QStringLiteral("textDocument/publishDiagnostics"))
        {
            // Analysis diagnostics (CE10): shown apart from the build report, which
            // stays the authority for build errors.
            const QJsonObject parameters = message.value(QStringLiteral("params")).toObject();
            const QJsonArray published = parameters.value(QStringLiteral("diagnostics")).toArray();
            QJsonArray diagnostics;
            for (const QJsonValue& diagnostic : published)
            {
                if (diagnostics.size() == MAX_PUBLISHED_DIAGNOSTICS)
                {
                    break;
                }
                diagnostics.append(diagnostic);
            }
            Q_EMIT DiagnosticsPublished(parameters.value(QStringLiteral("uri")).toString(),
                                        parameters.value(QStringLiteral("version")).toInt(-1),
                                        diagnostics);
        }
        return;
    }
    const int32 id = message.value(QStringLiteral("id")).toInt();
    if (id == mInitializeRequest && mInitializeRequest != 0)
    {
        receiveInitialize(message);
        return;
    }
    if (id == 0)
    {
        return;
    }
    for (usize index = 0; index < LANGUAGE_REQUEST_KIND_COUNT; ++index)
    {
        PendingRequest& request = mRequests[index];
        if (request.IsSent == false || request.Id != id)
        {
            continue;
        }
        // A cancelled or superseded request is no longer in the table, so its
        // late reply is discarded here.
        const LanguageRequestKind kind = static_cast<LanguageRequestKind>(index);
        request = {};
        armDeadline();
        if (message.contains(QStringLiteral("error")) == true)
        {
            if (message.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() ==
                METHOD_NOT_FOUND)
            {
                Q_EMIT RequestUnavailable(
                    kind,
                    QStringLiteral("The selected %1 does not implement that operation. Use Find for text matches; "
                                   "current source and selection were retained.")
                        .arg(mProfile->ServerName));
            }
            else
            {
                Q_EMIT RequestFailed(id,
                                     kind,
                                     QStringLiteral("%1 could not answer this request. Repair the %2 and retry.")
                                         .arg(mProfile->ProductName, mProfile->SyntaxNoun));
            }
        }
        else
        {
            Q_EMIT Replied(id, kind, message.value(QStringLiteral("result")));
        }
        return;
    }
}
} // namespace ludus::editor
