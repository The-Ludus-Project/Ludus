#include "internal/slang_language_service.h"

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

// Thanks to Microsoft, Language Server Protocol 3.17, Base Protocol, textDocument/
// completion and signatureHelp, and to the Slang contributors' language server.
// Use the compiler's semantic results rather than a second shader parser.
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
constexpr int32 INCREMENTAL_SYNC_KIND = 2;
constexpr int32 METHOD_NOT_FOUND = -32601;

QJsonObject GetPosition(const QString& text, int32 offset)
{
    const int32 line = static_cast<int32>(text.left(offset).count(QLatin1Char('\n')));
    const int32 previous = offset == 0 ? -1 : static_cast<int32>(text.lastIndexOf(QLatin1Char('\n'), offset - 1));
    return {{QStringLiteral("line"), line}, {QStringLiteral("character"), offset - previous - 1}};
}
} // namespace

SlangLanguageService::SlangLanguageService(QObject* parentOrNull) : QObject(parentOrNull)
{
#if !defined(Q_OS_WASM)
    connect(&mDecoder, &QFutureWatcher<QJsonDocument>::finished, this, [this]() {
        mIsDecoding = false;
        const QJsonDocument message = mDecoder.result();
        if (mDecodingGeneration == mGeneration)
        {
            if (message.isObject() == false)
            {
                fail(QStringLiteral("Invalid Slang response; retry assistance."));
                return;
            }
            receive(message.object());
        }
        QTimer::singleShot(0, this, &SlangLanguageService::readMessages);
    });
#endif
    mDeadline.setSingleShot(true);
    mDeadline.setInterval(REQUEST_TIMEOUT_MILLISECONDS);
    connect(&mDeadline, &QTimer::timeout, this, [this]() {
        fail(QStringLiteral("Shader assistance timed out. Retry completion to restart Slang."));
    });
}

SlangLanguageService::~SlangLanguageService()
{
    Stop();
}

void SlangLanguageService::Stop()
{
    ++mGeneration;
    mDeadline.stop();
    mIsReady = false;
    mCapabilities = {};
    mPendingRequest = 0;
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
        process->kill();
        // Keep teardown asynchronous too; the parent reaps a stopped child.
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
    }
#endif
}

void SlangLanguageService::Start(const QString& program, const QString& workspace, const QStringList& arguments)
{
    Stop();
#if defined(Q_OS_WASM)
    static_cast<void>(program);
    static_cast<void>(workspace);
    static_cast<void>(arguments);
    Q_EMIT Failed(QStringLiteral("Shader assistance needs native Slang; unavailable in the browser editor."));
#else
    mProcessOrNull = new QProcess(this);
    QProcess* process = mProcessOrNull;
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, process, [process]() {
        process->kill();
        static_cast<void>(process->waitForFinished(SHUTDOWN_WAIT_MILLISECONDS));
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &SlangLanguageService::readMessages);
    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        // Count/discard a bounded diagnostic stream, independently of JSON frames.
        process->setReadChannel(QProcess::StandardError);
        const QByteArray chunk = process->read(MAX_ERROR_BYTES - mErrorBytes + 1);
        mErrorBytes += chunk.size();
        process->setReadChannel(QProcess::StandardOutput);
        if (mErrorBytes > MAX_ERROR_BYTES)
        {
            fail(QStringLiteral("Slang exceeded its diagnostic budget. Check the selected tool and retry."));
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            fail(QStringLiteral("Cannot start slangd. Repair project shader tools or check the compiler override."));
        }
    });
    connect(process, &QProcess::finished, this, [this]() {
        fail(QStringLiteral("Slang stopped. Retry completion to restart shader assistance."));
    });
    connect(process, &QProcess::started, this, [this, workspace]() {
        mInitializeRequest = ++mNextRequest;
        const QJsonObject completionItem{{QStringLiteral("snippetSupport"), false},
                                         {QStringLiteral("insertReplaceSupport"), true}};
        const QJsonObject parameterInformation{{QStringLiteral("labelOffsetSupport"), true}};
        const QJsonObject signatureInformation{{QStringLiteral("parameterInformation"), parameterInformation}};
        const QJsonObject signatureHelp{{QStringLiteral("signatureInformation"), signatureInformation}};
        const QJsonObject textDocument{
            {QStringLiteral("documentSymbol"),
             QJsonObject{{QStringLiteral("hierarchicalDocumentSymbolSupport"), true}}},
            {QStringLiteral("definition"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
            {QStringLiteral("completion"), QJsonObject{{QStringLiteral("completionItem"), completionItem}}},
            {QStringLiteral("signatureHelp"), signatureHelp}};
        const QJsonObject capabilities{
            {QStringLiteral("general"),
             QJsonObject{{QStringLiteral("positionEncodings"), QJsonArray{QStringLiteral("utf-16")}}}},
            {QStringLiteral("workspace"), QJsonObject{{QStringLiteral("configuration"), true}}},
            {QStringLiteral("textDocument"), textDocument}};
        send({{QStringLiteral("id"), mInitializeRequest},
              {QStringLiteral("method"), QStringLiteral("initialize")},
              {QStringLiteral("params"),
               QJsonObject{{QStringLiteral("processId"), QJsonValue::Null},
                           {QStringLiteral("rootUri"), QUrl::fromLocalFile(workspace).toString()},
                           {QStringLiteral("capabilities"), capabilities}}}});
    });
    mDeadline.start();
    process->start(program, arguments);
#endif
}

void SlangLanguageService::fail(const QString& message)
{
    Stop();
    Q_EMIT Failed(message);
}

void SlangLanguageService::send(const QJsonObject& message)
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
        fail(QStringLiteral("Shader assistance exceeded its message budget. Reduce the shader context and retry."));
        return;
    }
    const QByteArray frame =
        QByteArrayLiteral("Content-Length: ") + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
    static_cast<void>(mProcessOrNull->write(frame));
#else
    static_cast<void>(message);
#endif
}

void SlangLanguageService::notify(const QString& method, const QJsonObject& parameters)
{
    send({{QStringLiteral("method"), method}, {QStringLiteral("params"), parameters}});
}

void SlangLanguageService::Cancel()
{
    if (mPendingRequest != 0)
    {
        notify(QStringLiteral("$/cancelRequest"), {{QStringLiteral("id"), mPendingRequest}});
        mPendingRequest = 0;
        mDeadline.stop();
    }
}

void SlangLanguageService::SetDocument(const QString& uri, const QString& text)
{
    mUri = uri;
    mText = text;
    if (mIsReady == true)
    {
        synchronize();
    }
}

void SlangLanguageService::synchronize()
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
                             {QStringLiteral("languageId"), QStringLiteral("slang")},
                             {QStringLiteral("version"), mVersion},
                             {QStringLiteral("text"), mText}}}});
        mSentUri = mUri;
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
            {QStringLiteral("start"), GetPosition(mSentText, prefix)},
            {QStringLiteral("end"), GetPosition(mSentText, static_cast<int32>(mSentText.size()) - suffix)}};
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

bool SlangLanguageService::CanRequest(const QString& method) const
{
    if (mIsReady == false)
    {
        return false;
    }
    const QString capability =
        method == QStringLiteral("textDocument/references")       ? QStringLiteral("referencesProvider")
        : method == QStringLiteral("textDocument/definition")     ? QStringLiteral("definitionProvider")
        : method == QStringLiteral("textDocument/documentSymbol") ? QStringLiteral("documentSymbolProvider")
                                                                  : QString{};
    if (capability.isEmpty() == true)
    {
        return true;
    }
    const QJsonValue value = mCapabilities.value(capability);
    return value.isObject() == true || value.toBool(false) == true;
}

int32 SlangLanguageService::Request(const QString& method, const QJsonObject& position)
{
    Cancel();
    if (mIsReady == false)
    {
        return 0;
    }
    if (CanRequest(method) == false)
    {
        Q_EMIT RequestUnavailable(QStringLiteral("The selected slangd does not support %1. Use Find for text matches; "
                                                 "current source and selection were retained.")
                                      .arg(method.section(QLatin1Char('/'), -1)));
        return 0;
    }
    mPendingRequest = ++mNextRequest;
    QJsonObject parameters{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), mUri}}}};
    if (method != QStringLiteral("textDocument/documentSymbol"))
    {
        parameters.insert(QStringLiteral("position"), position);
    }
    if (method == QStringLiteral("textDocument/references"))
    {
        parameters.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("includeDeclaration"), true}});
    }
    send({{QStringLiteral("id"), mPendingRequest},
          {QStringLiteral("method"), method},
          {QStringLiteral("params"), parameters}});
    mDeadline.start();
    return mPendingRequest;
}

void SlangLanguageService::readMessages()
{
#if !defined(Q_OS_WASM)
    if (mProcessOrNull == nullptr)
    {
        return;
    }
    if (mProcessOrNull->bytesAvailable() + mInput.size() > MAX_PROTOCOL_BYTES)
    {
        fail(QStringLiteral("Slang exceeded its protocol buffer budget; assistance stopped."));
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
        fail(QStringLiteral("Slang response exceeded its message budget; assistance stopped."));
        return;
    }
    const isize split = mInput.indexOf(QByteArrayLiteral("\r\n\r\n"));
    if (split < 0)
    {
        if (mInput.size() > MAX_HEADER_BYTES)
        {
            fail(QStringLiteral("Invalid Slang protocol header; retry assistance."));
        }
        return;
    }
    if (split > MAX_HEADER_BYTES)
    {
        fail(QStringLiteral("Slang protocol header exceeded its budget."));
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
        fail(QStringLiteral("Invalid Slang protocol length; retry assistance."));
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

void SlangLanguageService::receive(const QJsonObject& message)
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
                    const QString section = value.toObject().value(QStringLiteral("section")).toString();
                    values.append(section == QStringLiteral("slang.predefinedMacros") ||
                                          section == QStringLiteral("slang.additionalSearchPaths")
                                      ? QJsonValue(QJsonArray{})
                                  : section == QStringLiteral("slang.searchInAllWorkspaceDirectories")
                                      ? QJsonValue(false)
                                      : QJsonValue::Null);
                }
                result = values;
            }
            send({{QStringLiteral("id"), message.value(QStringLiteral("id"))}, {QStringLiteral("result"), result}});
        }
        return; // background diagnostics are deliberately not a build-report authority
    }
    const int32 request = message.value(QStringLiteral("id")).toInt();
    if (request == mInitializeRequest && mInitializeRequest != 0)
    {
        const QJsonObject capabilities =
            message.value(QStringLiteral("result")).toObject().value(QStringLiteral("capabilities")).toObject();
        const QString encoding =
            capabilities.value(QStringLiteral("positionEncoding")).toString(QStringLiteral("utf-16"));
        if (message.contains(QStringLiteral("error")) == true || encoding != QStringLiteral("utf-16") ||
            capabilities.value(QStringLiteral("textDocumentSync")).toObject().value(QStringLiteral("change")).toInt() !=
                INCREMENTAL_SYNC_KIND ||
            capabilities.contains(QStringLiteral("completionProvider")) == false ||
            capabilities.contains(QStringLiteral("signatureHelpProvider")) == false)
        {
            fail(QStringLiteral("This slangd does not support the required UTF-16 completion/signature protocol. Use "
                                "the project's pinned tools."));
            return;
        }
        mInitializeRequest = 0;
        mCapabilities = capabilities;
        mDeadline.stop();
        mIsReady = true;
        notify(QStringLiteral("initialized"), {});
        if (mUri.isEmpty() == false)
        {
            synchronize();
        }
        Q_EMIT Ready();
    }
    else if (request == mPendingRequest && mPendingRequest != 0)
    {
        mPendingRequest = 0;
        mDeadline.stop();
        if (message.contains(QStringLiteral("error")) == true)
        {
            if (message.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() ==
                METHOD_NOT_FOUND)
            {
                Q_EMIT RequestUnavailable(
                    QStringLiteral("The selected slangd does not implement that operation. Use Find for text matches; "
                                   "current source and selection were retained."));
            }
            else
            {
                Q_EMIT Failed(
                    QStringLiteral("Slang could not answer this request. Repair the shader syntax and retry."));
            }
        }
        else
        {
            Q_EMIT Replied(request, message.value(QStringLiteral("result")));
        }
    }
}
} // namespace ludus::editor
