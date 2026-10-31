#include "internal/render_graph_shader_assistance.h"

#include "internal/code_text.h"
#include "internal/render_graph_document.h"

#include <QByteArrayView>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 PREPARATION_TIMEOUT_MILLISECONDS = 15000;
#if !defined(Q_OS_WASM)
constexpr isize MAX_DRAFT_INPUT_BYTES = LUDUS_EDITOR_SHADER_CONTEXT_INPUT_BYTES;
constexpr isize JSON_CONTAINER_EDGE_BYTES = 2;
constexpr isize JSON_STRING_EDGE_BYTES = 2;
constexpr isize JSON_SEPARATOR_BYTES = 1;
#endif
} // namespace

RenderGraphShaderAssistance::RenderGraphShaderAssistance(RenderGraphDocument& inoutDocument,
                                                         CodeEditor& inoutEditor,
                                                         CodeDocumentController& inoutController,
                                                         const ToolingPaths& tooling,
                                                         QWidget* parentOrNull)
    : QWidget(parentOrNull), mDocument(inoutDocument), mAssistance(inoutEditor, inoutController, this),
      mTooling(tooling), mJob(this), mContexts(new QComboBox(this)), mStatus(new QLabel(this))
{
    setObjectName(QStringLiteral("renderGraphShaderAssistance"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* row = new QHBoxLayout;
    QLabel* label = new QLabel(QStringLiteral("Shader context"), this);
    label->setBuddy(mContexts);
    row->addWidget(label);
    mContexts->setObjectName(QStringLiteral("shaderProgramContext"));
    mContexts->setAccessibleName(QStringLiteral("Shader program context"));
    mContexts->setEditable(true);
    mContexts->lineEdit()->setReadOnly(true);
    mContexts->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mContexts->setPlaceholderText(QStringLiteral("Ctrl+Space: complete · Ctrl+Shift+Space: signature help"));
    row->addWidget(mContexts, 1);
    QPushButton* retry = new QPushButton(QStringLiteral("Retry assistance"), this);
    retry->setToolTip(
        QStringLiteral("Recreate the current draft's compiler context and restart failed shader assistance."));
    row->addWidget(retry);
    layout->addLayout(row);
    mStatus->setObjectName(QStringLiteral("shaderAssistanceStatus"));
    mStatus->setTextFormat(Qt::PlainText);
    mStatus->setWordWrap(false);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    mStatus->setFixedHeight(mStatus->fontMetrics().height());
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(mStatus);
    connect(&mAssistance, &CodeAssistance::StatusChanged, this, &RenderGraphShaderAssistance::setStatus);
    connect(&mAssistance, &CodeAssistance::ContextRequested, this, &RenderGraphShaderAssistance::prepare);
    connect(&mAssistance, &CodeAssistance::ContextCancelled, this, [this]() {
        ++mNextRequest;
        mHasPending = false;
        mDeadline.stop();
        mJob.Cancel();
    });
    connect(mContexts, &QComboBox::activated, this, [this](int32 index) {
        mSelectedContext = mContexts->itemData(index).toString();
        mAssistance.ClearContext();
        setStatus(QStringLiteral("Program context changed. Complete a symbol to prepare it."));
    });
    connect(retry, &QPushButton::clicked, this, [this, &inoutEditor]() {
        mSelectedContext.clear();
        mContexts->clear();
        inoutEditor.setFocus();
        mAssistance.Request(false);
    });
    connect(&mJob, &RenderGraphJob::Finished, this, &RenderGraphShaderAssistance::accept);
    mDeadline.setSingleShot(true);
    mDeadline.setInterval(PREPARATION_TIMEOUT_MILLISECONDS);
    connect(&mDeadline, &QTimer::timeout, this, [this]() {
        mHasPending = false;
        mJob.Cancel();
        mAssistance.Cancel();
        setStatus(QStringLiteral("Shader context preparation timed out. Check the graph and Retry assistance."));
    });
#if defined(Q_OS_WASM)
    retry->setEnabled(false);
    mContexts->setEnabled(false);
    setStatus(QStringLiteral("Native Slang completion and signature help are unavailable in the browser editor."));
#endif
}

void RenderGraphShaderAssistance::setStatus(const QString& message)
{
    mStatus->setText(message.simplified());
    mStatus->setToolTip(message);
}

QByteArray RenderGraphShaderAssistance::getIdentity() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QJsonObject scope{{QStringLiteral("project"), mProject},
                            {QStringLiteral("epoch"), QStringLiteral("%1").arg(mEpoch)},
                            {QStringLiteral("manifest"), mManifest},
                            {QStringLiteral("source"), mFile},
                            {QStringLiteral("helper"), mSettings.Tools.Helper},
                            {QStringLiteral("compiler"), mSettings.Tools.Compiler},
                            {QStringLiteral("override"), mSettings.ShouldOverrideTools}};
    hash.addData(QJsonDocument(scope).toJson(QJsonDocument::Compact));
    for (const QString& file : mDocument.GetFiles())
    {
        hash.addData(file.toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(mDocument.GetDraft(file));
    }
    return hash.result();
}

void RenderGraphShaderAssistance::Update(const QString& project,
                                         uint64 epoch,
                                         const QString& manifest,
                                         const QString& file,
                                         const RenderGraphBuildSettings& settings)
{
    if (project != mProject || epoch != mEpoch || manifest != mManifest || file != mFile || settings != mSettings)
    {
        ++mNextRequest;
        mHasPending = false;
        mDeadline.stop();
        mJob.Cancel();
        mAssistance.ClearContext();
        mResult = {};
        mSelectedContext.clear();
        mContexts->clear();
        mProject = project;
        mEpoch = epoch;
        mManifest = manifest;
        mFile = file;
        mSettings = settings;
#if defined(Q_OS_WASM)
        setStatus(QStringLiteral("Native Slang completion and signature help are unavailable in the browser editor."));
#else
        setStatus({});
#endif
    }
    setVisible(file.endsWith(QStringLiteral(".slang")));
}

void RenderGraphShaderAssistance::prepare()
{
#if !defined(Q_OS_WASM)
    const QByteArray identity = getIdentity();
    if (mJob.IsActive() == true)
    {
        // Only one captured-package generation runs at once. A later intent is
        // rechecked against the current package after this child exits.
        mHasPending = true;
        return;
    }
    mHasPending = false;
    mResult = {};
    if (mTemporary.isValid() == false)
    {
        mAssistance.Cancel();
        setStatus(QStringLiteral("Cannot capture the shader draft. Check temporary storage and retry."));
        return;
    }
    RenderGraphJobRequest request;
    request.Request = ++mNextRequest;
    request.Revision = mDocument.GetRevision();
    request.Action = QStringLiteral("shader-context");
    request.InputIdentity = identity;
    request.ProjectEpoch = mEpoch;
    // Capture owned bytes only. The background compiler-context owner validates,
    // writes and removes its private staging files; typing performs no file writes.
    QJsonObject files;
    isize estimatedBytes =
        QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("files"), QJsonObject{}}})
            .toJson(QJsonDocument::Compact)
            .size();
    bool isFirst = true;
    for (const QString& file : mDocument.GetFiles())
    {
        const QByteArray encoded = mDocument.GetDraft(file).toBase64();
        estimatedBytes += QJsonDocument(QJsonArray{file}).toJson(QJsonDocument::Compact).size() -
                          JSON_CONTAINER_EDGE_BYTES + JSON_SEPARATOR_BYTES + encoded.size() + JSON_STRING_EDGE_BYTES +
                          (isFirst == true ? 0 : JSON_SEPARATOR_BYTES);
        if (estimatedBytes > MAX_DRAFT_INPUT_BYTES)
        {
            mAssistance.Cancel();
            setStatus(QStringLiteral("The captured shader package exceeds assistance's input limit. Reduce its "
                                     "size or path lengths and retry."));
            return;
        }
        isFirst = false;
        files.insert(file, QString::fromLatin1(encoded));
    }
    request.StandardInput = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("files"), files}})
                                .toJson(QJsonDocument::Compact);
    if (request.StandardInput.size() > MAX_DRAFT_INPUT_BYTES)
    {
        mAssistance.Cancel();
        setStatus(
            QStringLiteral("The captured shader package exceeds assistance's input limit. Reduce its size and retry."));
        return;
    }
    const QString input = mTemporary.filePath(QStringLiteral("input-%1").arg(request.Request));
    request.Arguments = {QDir(input).filePath(mManifest),
                         mTemporary.filePath(QStringLiteral("language")),
                         QStringLiteral("--root"),
                         input,
                         QStringLiteral("--draft-stdin"),
                         QStringLiteral("--source"),
                         mFile};
    if (mSelectedContext.isEmpty() == false)
    {
        request.Arguments += {QStringLiteral("--context"), mSelectedContext};
    }
    if (mSettings.ShouldOverrideTools == true)
    {
        request.Arguments += {QStringLiteral("--helper"),
                              mSettings.Tools.Helper,
                              QStringLiteral("--compiler"),
                              mSettings.Tools.Compiler};
    }
    else
    {
        request.Arguments +=
            {QStringLiteral("--project"), mProject, QStringLiteral("--tooling-root"), mTooling.ToolingRoot};
    }
    setStatus(QStringLiteral("Preparing shader context from this draft…"));
    if (mJob.Start(mTooling, request) == false)
    {
        mAssistance.Cancel();
        setStatus(QStringLiteral("Cannot run shader context tooling. Repair project setup and retry."));
        return;
    }
    mDeadline.start();
#endif
}

void RenderGraphShaderAssistance::accept(const RenderGraphJobResult& result)
{
    mDeadline.stop();
    if (mHasPending == true)
    {
        mHasPending = false;
        prepare();
        return;
    }
    if (result.IsCancelled == true || result.Request.Request != mNextRequest || result.Request.ProjectEpoch != mEpoch)
    {
        return;
    }
    if (result.Request.Revision != mDocument.GetRevision() || result.Request.InputIdentity != getIdentity())
    {
        setStatus(QStringLiteral(
            "Shader inputs changed while preparing assistance. Request completion or signature help again."));
        return;
    }
    if (result.IsSuccessful == false)
    {
        mAssistance.Cancel();
        setStatus(result.Message +
                  QStringLiteral(" Repair the graph/shader inputs or tool selection, then Retry assistance."));
        return;
    }
    mResult = result.Payload;
    mSelectedContext = mResult.value(QStringLiteral("context")).toString();
    const QSignalBlocker blocker(mContexts);
    mContexts->clear();
    for (const QJsonValue& value : mResult.value(QStringLiteral("contexts")).toArray())
    {
        const QJsonObject context = value.toObject();
        const QString identity = context.value(QStringLiteral("id")).toString();
        QStringList programs;
        for (const QJsonValue& program : context.value(QStringLiteral("programs")).toArray())
        {
            programs.append(program.toString());
        }
        const QString authored = programs.join(QStringLiteral(", "));
        const QString label = (authored.isEmpty() == true ? QString() : authored + QStringLiteral(" · ")) + identity +
                              QStringLiteral(" · ") + context.value(QStringLiteral("stage")).toString();
        mContexts->addItem(label, identity);
        mContexts->setItemData(mContexts->count() - 1,
                               QStringLiteral("Manifest: %1\nAuthored program IDs: %2\nLinked compiler unit: %3")
                                   .arg(mManifest, authored, identity),
                               Qt::ToolTipRole);
        if (identity == mSelectedContext)
        {
            mContexts->setCurrentIndex(mContexts->count() - 1);
        }
    }
    mContexts->setToolTip(QStringLiteral("Linked compiler context in %1. IDs belong to this package-wide manifest; "
                                         "graph node IDs do not scope them. Select and copy the full context ID.")
                              .arg(mManifest));
    publish();
}

void RenderGraphShaderAssistance::publish()
{
    const QUrl uri(mResult.value(QStringLiteral("uri")).toString());
    const QString path = QFileInfo(uri.toLocalFile()).canonicalFilePath();
    const QString temporary = QFileInfo(mTemporary.path()).canonicalFilePath() + QLatin1Char('/');
    const QString server = mResult.value(QStringLiteral("server")).toString();
    if (mResult.value(QStringLiteral("version")).toInt() != 1 || uri.isLocalFile() == false ||
        path.startsWith(temporary) == false)
    {
        mAssistance.Cancel();
        setStatus(
            QStringLiteral("Shader assistance needs a readable generated context and executable slangd beside the "
                           "selected slangc. Repair project tools or the compiler override, then retry."));
        return;
    }
    if (QFileInfo(server).isExecutable() == false)
    {
        mAssistance.Cancel();
        setStatus(QStringLiteral("Missing executable slangd beside the selected slangc. Repair project shader tools "
                                 "or the compiler override, then Retry assistance."));
        return;
    }
    QString prefix = mResult.value(QStringLiteral("prefix")).toString();
    QString suffix = mResult.value(QStringLiteral("suffix")).toString();
    prefix.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    prefix.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    suffix.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    suffix.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QMap<QString, CodeNavigationSource> navigationSources;
    qsizetype totalBytes = 0;
    for (const QJsonValue& value : mResult.value(QStringLiteral("navigation_sources")).toArray())
    {
        const QJsonObject entry = value.toObject();
        const QUrl sourceUri(entry.value(QStringLiteral("uri")).toString());
        const QFileInfo information(sourceUri.toLocalFile());
        const QString capturedPath = information.canonicalFilePath();
        if (sourceUri.isLocalFile() == false || information.isSymLink() == true ||
            capturedPath.startsWith(temporary) == false || information.size() > RenderGraphDocument::MAX_FILE_BYTES)
        {
            continue;
        }
        QFile input(capturedPath);
        if (input.open(QIODevice::ReadOnly) == false)
        {
            continue;
        }
        const QByteArray bytes = input.read(RenderGraphDocument::MAX_FILE_BYTES + 1);
        totalBytes += bytes.size();
        if (input.error() != QFileDevice::NoError || bytes.size() > RenderGraphDocument::MAX_FILE_BYTES ||
            totalBytes > RenderGraphDocument::MAX_TOTAL_BYTES)
        {
            break;
        }
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() !=
            entry.value(QStringLiteral("sha256")).toString().toLatin1())
        {
            continue;
        }
        const QString authored = entry.value(QStringLiteral("file")).toString();
        const bool isAuthored =
            mDocument.GetFiles().contains(authored) == true && mDocument.GetDraft(authored) == bytes;
        QString text;
        if (DecodeCodeText(bytes, text) == false)
        {
            continue;
        }
        navigationSources.insert(sourceUri.toString(), {isAuthored == true ? authored : QString{}, text});
    }
    mAssistance.SetContext(server,
                           mTemporary.filePath(QStringLiteral("language")),
                           uri.toString(),
                           prefix,
                           suffix,
                           {},
                           navigationSources);
}
} // namespace ludus::editor
