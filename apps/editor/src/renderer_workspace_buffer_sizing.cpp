#include "internal/code_editor.h"
#include "internal/code_editor_area.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/render_graph_node_details.h"
#include "internal/render_graph_shader_edits.h"
#include "internal/render_graph_shader_language.h"
#include "internal/renderer_workspace.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/base/types.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>

namespace ludus::editor
{
namespace
{
namespace format = content::render_graph_format;

const QString BUFFER_LAYOUT_ALIGNMENT_NOTE =
    QStringLiteral("Total capacity rounds up to a %1-byte buffer word.").arg(format::RAY_BUFFER_WORD_BYTES);
constexpr foundation::int32 DECLARATION_EDITOR_LINES = 12;

// Each surface owns its pending preview. Results never enter the graph build or document history.
// The document and tooling belong to the workspace. A rebuilt Properties panel detaches its old
// rows and deletes them later, so a preview can outlive the workspace by one event-loop pass;
// its timer and job must then stop instead of reading the destroyed document.
class BufferLayoutPreview final : public QLabel
{
public:
    BufferLayoutPreview(const RendererWorkspace& owner,
                        const RenderGraphDocument& document,
                        const ToolingPaths& tooling,
                        const QString& manifest,
                        const QStringList& toolArguments,
                        const QString& heading,
                        QWidget& inoutParent)
        : QLabel(&inoutParent), mOwnerOrNull(&owner), mDocument(document), mTooling(tooling), mManifest(manifest),
          mToolArguments(toolArguments), mHeading(heading)
    {
        setTextFormat(Qt::PlainText);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setWordWrap(true);
        mDebounce.setSingleShot(true);
        constexpr foundation::int32 REFLECTION_DEBOUNCE_MILLISECONDS = 150;
        mDebounce.setInterval(REFLECTION_DEBOUNCE_MILLISECONDS);
        connect(&mDebounce, &QTimer::timeout, this, [this]() { startReflection(); });
        connect(&mJob, &RenderGraphJob::Finished, this, [this](const RenderGraphJobResult& result) {
            if (isOwnerAlive() == false)
            {
                return;
            }
            if (result.Request.Request == mGeneration && result.Request.Revision == mDocument.GetRevision())
            {
                mStride = result.Payload.value(QStringLiteral("buffer_layout"))
                              .toObject()
                              .value(QStringLiteral("stride"))
                              .toInteger();
                if (result.IsSuccessful == false || mStride <= 0 || mStride > format::MAX_RAY_BUFFER_BYTES)
                {
                    mStride = 0;
                    mProblem = result.Message.isEmpty() == false
                                   ? result.Message
                                   : QStringLiteral("The compiler returned no usable stride. Check the declaration.");
                }
                render();
            }
            else
            {
                RefreshDraft();
                if (mType.isEmpty() == false && mDebounce.isActive() == false)
                {
                    mDebounce.start();
                }
            }
        });
    }

    void SetRecord(const QString& source, const QString& type)
    {
        if (source == mSource && type == mType)
        {
            return;
        }
        mSource = source;
        mType = type;
        refresh();
    }

    void SetCount(foundation::int64 count)
    {
        mCount = count;
        render();
    }

    void RefreshDraft()
    {
        if (isOwnerAlive() == true && mRevision != mDocument.GetRevision())
        {
            refresh();
        }
    }

private:
    [[nodiscard]] bool isOwnerAlive()
    {
        if (mOwnerOrNull.isNull() == false)
        {
            return true;
        }
        mDebounce.stop();
        return false;
    }

    void refresh()
    {
        ++mGeneration;
        mRevision = mDocument.GetRevision();
        mStride = 0;
        mProblem.clear();
        if (mType.isEmpty() == true)
        {
            mDebounce.stop();
        }
        else
        {
            mDebounce.start();
        }
        render();
    }

    void startReflection()
    {
        if (isOwnerAlive() == false || mJob.IsActive() == true)
        {
            return;
        }
        if (mType.isEmpty() == true)
        {
            return;
        }
        const QString root = mTemporary.filePath(QStringLiteral("draft"));
        if (mTemporary.isValid() == false || QDir(root).removeRecursively() == false || QDir().mkpath(root) == false ||
            mDocument.WriteSnapshot(root) != RenderGraphDocumentStatus::Ok)
        {
            mProblem = QStringLiteral("Could not capture the draft. Check available temporary storage.");
            render();
            return;
        }
        RenderGraphJobRequest request;
        request.Request = mGeneration;
        request.Revision = mRevision;
        request.Action = QStringLiteral("buffer-layout");
        request.Arguments = {QDir(root).filePath(mManifest),
                             QStringLiteral("--root"),
                             root,
                             QStringLiteral("--source"),
                             mSource,
                             QStringLiteral("--type"),
                             mType};
        request.Arguments += mToolArguments;
        if (mJob.Start(mTooling, request) == false)
        {
            mProblem = QStringLiteral("Shader reflection tooling is unavailable. Check the editor's shader tools.");
            render();
        }
    }

    void render()
    {
        QString description;
        if (mType.isEmpty() == false)
        {
            if (mProblem.isEmpty() == false)
            {
                description =
                    QStringLiteral("Size unavailable. %1").arg(mProblem.trimmed().section(QLatin1Char('\n'), 0, 0));
            }
            else if (mStride == 0)
            {
                description = QStringLiteral("Reflecting storage layout…");
            }
            else
            {
                const foundation::int64 word = format::RAY_BUFFER_WORD_BYTES;
                const foundation::int64 bytes = ((mStride * mCount + word - 1) / word) * word;
                description = QStringLiteral("%L1 bytes per element · %L2 bytes total").arg(mStride).arg(bytes);
                if (bytes > format::MAX_RAY_BUFFER_BYTES)
                {
                    description += QStringLiteral("\nLimit is %L1 bytes. Reduce the element capacity or struct size.")
                                       .arg(format::MAX_RAY_BUFFER_BYTES);
                }
            }
        }
        setToolTip(mProblem);
        setText(mHeading.isEmpty() == true ? description : mHeading + QStringLiteral("\n") + description);
    }

    QPointer<const RendererWorkspace> mOwnerOrNull;
    const RenderGraphDocument& mDocument;
    const ToolingPaths& mTooling;
    QString mManifest;
    QStringList mToolArguments;
    QString mHeading;
    QString mSource;
    QString mType;
    QString mProblem;
    foundation::int64 mCount = 1;
    foundation::int64 mStride = 0;
    foundation::uint64 mGeneration = 0;
    foundation::uint64 mRevision = 0;
    QTemporaryDir mTemporary;
    QTimer mDebounce;
    RenderGraphJob mJob;
};

QStringList FindDeclaredStructs(const QByteArray& source)
{
    // The picker lists local declarations. Compile Shaders preprocesses the source and verifies its storage layout.
    static const QRegularExpression COMMENTS_AND_STRINGS(
        QStringLiteral(R"expression(/\*[\s\S]*?(?:\*/|\z)|//[^\r\n]*|"(?:\\.|[^"\\])*")expression"));
    static const QRegularExpression STRUCTURE_OR_BRACE(
        QStringLiteral("\\bstruct\\s+([A-Za-z][A-Za-z0-9_]{0,%1})\\s*\\{|[{}]")
            .arg(RenderGraphDocument::MAX_IDENTIFIER_LENGTH - 1));
    QString text = QString::fromUtf8(source);
    text.replace(COMMENTS_AND_STRINGS, QStringLiteral(" "));
    QStringList names;
    foundation::int32 depth = 0;
    QRegularExpressionMatchIterator tokens = STRUCTURE_OR_BRACE.globalMatch(text);
    while (tokens.hasNext() == true)
    {
        const QRegularExpressionMatch token = tokens.next();
        if (token.capturedView(0).endsWith(QLatin1Char('{')) == true)
        {
            const QString name = token.captured(1);
            if (depth == 0 && name.isEmpty() == false)
            {
                names.append(name);
            }
            ++depth;
        }
        else if (depth > 0)
        {
            --depth;
        }
    }
    names.removeDuplicates();
    return names;
}

QStringList FindSharedShaderIncludes(const RenderGraphDocument& document, const QString& target)
{
    QStringList headers;
    for (const QString& file : document.GetFiles())
    {
        if (GetSharedShaderIncludePath(target, file).isEmpty() == false)
        {
            headers.append(file);
        }
    }
    return headers;
}

QString ChooseSharedShaderInclude(QWidget& inoutParent, const QStringList& headers, const QString& target)
{
    const QString header = QInputDialog::getItem(&inoutParent,
                                                 QStringLiteral("Insert Shared Include"),
                                                 QStringLiteral("Header in this graph package"),
                                                 headers,
                                                 0,
                                                 false);
    return header.isEmpty() == true ? QString() : GetSharedShaderIncludePath(target, header);
}

CodeEditor* AddDeclarationEditor(QFormLayout& outFields, QDialog& inoutDialog, const QString& text)
{
    CodeEditor* editor = new CodeEditor(&inoutDialog);
    editor->setObjectName(QStringLiteral("renderGraphBufferDeclarationEditor"));
    editor->setAccessibleName(QStringLiteral("Shader declaration source"));
    editor->setMinimumHeight(editor->fontMetrics().lineSpacing() * DECLARATION_EDITOR_LINES);
    editor->setPlainText(text);
    outFields.addRow(editor);
    return editor;
}

bool ReadBufferDeclaration(CodeEditor& inoutEditor, QLabel& outProblem, QByteArray& outSource)
{
    const QByteArray source = inoutEditor.toPlainText().toUtf8();
    if (source.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        outProblem.setText(DescribePackageCapacity());
    }
    else if (FindDeclaredStructs(source).isEmpty() == true)
    {
        outProblem.setText(QStringLiteral("Add a top-level struct definition with a body to use for buffer sizing."));
    }
    else
    {
        outSource = source;
        return true;
    }
    inoutEditor.setFocus();
    return false;
}

} // namespace

QString RendererWorkspace::stageSharedShaderSource(QWidget& inoutParent,
                                                   const QString& nearbyFile,
                                                   SharedShaderFilePurpose purpose)
{
    if (mDocument.IsLoaded() == false)
    {
        return {};
    }

    QDialog dialog(&inoutParent);
    dialog.setWindowTitle(QStringLiteral("New shared shader file"));
    QFormLayout fields(&dialog);
    const bool isBufferLayout = purpose == SharedShaderFilePurpose::BufferLayout;
    const RenderGraphShaderLanguage defaultLanguage =
        isBufferLayout == false && nearbyFile.endsWith(GetShaderSourceSuffix(RenderGraphShaderLanguage::Slang)) == true
            ? RenderGraphShaderLanguage::Slang
            : RenderGraphShaderLanguage::Lusl;
    QComboBox* language = new QComboBox(&dialog);
    language->setObjectName(QStringLiteral("renderGraphSharedShaderLanguage"));
    language->addItem(QStringLiteral("LUSL include (%1)").arg(GetShaderIncludeSuffix(RenderGraphShaderLanguage::Lusl)),
                      QVariant::fromValue(RenderGraphShaderLanguage::Lusl));
    language->addItem(
        QStringLiteral("Slang include (%1)").arg(GetShaderIncludeSuffix(RenderGraphShaderLanguage::Slang)),
        QVariant::fromValue(RenderGraphShaderLanguage::Slang));
    language->setCurrentIndex(language->findData(QVariant::fromValue(defaultLanguage)));
    fields.addRow(QStringLiteral("Language"), language);

    const QDir directory(QFileInfo(nearbyFile).path());
    const QString defaultPath =
        QDir::cleanPath(directory.filePath(QStringLiteral("shared") + GetShaderIncludeSuffix(defaultLanguage)));
    QLineEdit* path = new QLineEdit(defaultPath, &dialog);
    path->setObjectName(QStringLiteral("renderGraphSharedShaderPath"));
    path->selectAll();
    fields.addRow(QStringLiteral("Package-relative path"), path);
    QCheckBox* include = new QCheckBox(QStringLiteral("Include from %1").arg(nearbyFile), &dialog);
    include->setObjectName(QStringLiteral("renderGraphIncludeNewSharedShader"));
    include->setToolTip(
        QStringLiteral("Link the new file from the current shader when their languages and paths are compatible."));
    include->setVisible(isBufferLayout == false && nearbyFile.isEmpty() == false);
    const auto updateInclude = [include, path, nearbyFile]() {
        include->setEnabled(GetSharedShaderIncludePath(nearbyFile, path->text().trimmed()).isEmpty() == false);
    };
    include->setChecked(true);
    updateInclude();
    connect(path, &QLineEdit::textChanged, &dialog, updateInclude);
    fields.addRow(include);
    const QString usage =
        isBufferLayout == true
            ? QStringLiteral("Define the buffer struct below. The file joins the graph draft when you create it.")
            : QStringLiteral("Declare shared types and helpers here. Use a quoted #include from the shaders "
                             "that need them, or let Ludus add the include below. Save writes the graph draft.");
    QLabel* hint = new QLabel(usage, &dialog);
    hint->setObjectName(QStringLiteral("renderGraphSharedShaderHint"));
    hint->setWordWrap(true);
    fields.addRow(hint);
    CodeEditor* editorOrNull = nullptr;
    if (isBufferLayout == true)
    {
        editorOrNull = AddDeclarationEditor(fields, dialog, QStringLiteral("#pragma once\n\n"));
        editorOrNull->SetLanguage(CodeLanguage::Lusl);
    }
    QLabel* problem = new QLabel(&dialog);
    problem->setObjectName(QStringLiteral("renderGraphSharedShaderError"));
    problem->setTextFormat(Qt::PlainText);
    problem->setWordWrap(true);
    fields.addRow(problem);
    connect(path, &QLineEdit::textEdited, problem, &QLabel::clear);
    if (editorOrNull != nullptr)
    {
        connect(editorOrNull, &CodeEditor::textChanged, problem, &QLabel::clear);
    }
    connect(language, &QComboBox::currentIndexChanged, &dialog, [language, path, problem, editorOrNull]() {
        const RenderGraphShaderLanguage selected = language->currentData().value<RenderGraphShaderLanguage>();
        if (editorOrNull != nullptr)
        {
            editorOrNull->SetLanguage(selected == RenderGraphShaderLanguage::Lusl ? CodeLanguage::Lusl
                                                                                  : CodeLanguage::Slang);
        }
        const RenderGraphShaderLanguage other = selected == RenderGraphShaderLanguage::Lusl
                                                    ? RenderGraphShaderLanguage::Slang
                                                    : RenderGraphShaderLanguage::Lusl;
        QString file = path->text().trimmed();
        const QString otherSuffix = GetShaderIncludeSuffix(other);
        if (file.endsWith(otherSuffix) == true)
        {
            file.chop(otherSuffix.size());
            path->setText(file + GetShaderIncludeSuffix(selected));
        }
        problem->clear();
    });
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton* create = buttons->addButton(QStringLiteral("Create Shared File"), QDialogButtonBox::AcceptRole);
    create->setObjectName(QStringLiteral("renderGraphCreateSharedShader"));
    fields.addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(create,
            &QPushButton::clicked,
            &dialog,
            [this, path, language, problem, editorOrNull, include, isBufferLayout, nearbyFile, &dialog]() {
                QByteArray source;
                if (editorOrNull != nullptr && ReadBufferDeclaration(*editorOrNull, *problem, source) == false)
                {
                    return;
                }
                QString error;
                const RenderGraphShaderLanguage selected = language->currentData().value<RenderGraphShaderLanguage>();
                const QString file = path->text().trimmed();
                const QString target =
                    isBufferLayout == false && include->isEnabled() == true && include->isChecked() == true ? nearbyFile
                                                                                                            : QString();
                const RenderGraphDocumentStatus status =
                    mDocument.CreateSharedShaderSource(file, selected, error, source, target);
                if (status == RenderGraphDocumentStatus::Ok)
                {
                    dialog.accept();
                    return;
                }
                if (editorOrNull != nullptr && status == RenderGraphDocumentStatus::Conflict &&
                    mDocument.GetFiles().contains(file, Qt::CaseInsensitive) == true)
                {
                    error =
                        QStringLiteral("'%1' already exists. Choose another path, or cancel and select it in Buffer "
                                       "sizing to edit its declaration.")
                            .arg(file);
                }
                problem->setText(error);
                path->setFocus();
            });
    path->setFocus();
    return dialog.exec() == QDialog::Accepted ? path->text().trimmed() : QString();
}

void RendererWorkspace::openNewSharedShaderSource(QWidget& inoutParent, const QString& nearbyFile)
{
    const QString file = stageSharedShaderSource(inoutParent, nearbyFile, SharedShaderFilePurpose::Include);
    if (file.isEmpty() == true)
    {
        return;
    }
    QDialog* detailsOrNull = qobject_cast<QDialog*>(inoutParent.window());
    if (detailsOrNull != nullptr)
    {
        detailsOrNull->accept();
    }
    documentEdited(RenderGraphDocumentStatus::Ok);
    openSourceLine(file, 1);
}

QString RendererWorkspace::insertSharedShaderInclude(QWidget& inoutParent, const QString& target)
{
    const QStringList headers = FindSharedShaderIncludes(mDocument, target);
    if (headers.isEmpty() == true)
    {
        return QStringLiteral("No compatible shared header is available for this source. "
                              "Use Source → New shared shader file to create one.");
    }
    const QString package = mDocument.GetPackageRoot();
    const foundation::uint64 revision = mDocument.GetRevision();
    const QString include = ChooseSharedShaderInclude(inoutParent, headers, target);
    if (include.isEmpty() == true)
    {
        return {};
    }
    if (package != mDocument.GetPackageRoot() || revision != mDocument.GetRevision())
    {
        return QStringLiteral("The shader draft changed. Review the current source and try again.");
    }
    const QByteArray before = mDocument.GetDraft(target);
    const QByteArray after = InsertShaderInclude(before, include);
    if (after == before)
    {
        return QStringLiteral("This source already includes '%1'.").arg(include);
    }
    const RenderGraphDocumentStatus status = mDocument.SetText(target, after);
    documentEdited(status);
    return status == RenderGraphDocumentStatus::Ok
               ? QString()
               : (status == RenderGraphDocumentStatus::Capacity
                      ? DescribePackageCapacity()
                      : QStringLiteral("Could not insert the include. Reopen the shader and try again."));
}

void RendererWorkspace::addShaderBufferAccess()
{
    const QString file = GetSourceFile();
    const QString manifest = manifestFile();
    const QByteArray source = mDocument.GetDraft(file);
    const foundation::uint64 revision = mDocument.GetRevision();
    const QJsonArray callbacks = FindShaderBufferCallbacks(mDocument.GetDraft(manifest), file);
    const bool isLusl = file.endsWith(QStringLiteral(".lusl"));
    QStringList functions;
    for (const QJsonValueConstRef entry : callbacks)
    {
        const QJsonObject program = entry.toObject();
        const QString function = program.value(QStringLiteral("function")).toString();
        if (functions.contains(function) == false)
        {
            functions.append(function);
        }
    }
    if (functions.isEmpty() == true)
    {
        return;
    }
    const QString function = functions.size() == 1 ? functions.constFirst()
                                                   : QInputDialog::getItem(this,
                                                                           QStringLiteral("Add Resource Access"),
                                                                           QStringLiteral("Callback in %1").arg(file),
                                                                           functions,
                                                                           0,
                                                                           false);
    if (function.isEmpty() == true)
    {
        return;
    }
    QString error;
    const QByteArray updated = AddShaderBufferParameter(source, function, isLusl, error);
    if (updated.isNull() == true)
    {
        mSourceStatus->setText(error);
        return;
    }
    QMessageBox confirm(QMessageBox::Question,
                        QStringLiteral("Add Resource Access"),
                        QStringLiteral("Enable named resource access for %1 in %2?").arg(function, file),
                        QMessageBox::Cancel,
                        this);
    confirm.setTextFormat(Qt::PlainText);
    QString description = isLusl == true
                              ? QStringLiteral("Adds the generated include and the resource parameter where needed.")
                              : QStringLiteral("Adds the resource parameter where needed.");
    for (const QJsonValueConstRef entry : callbacks)
    {
        const QJsonObject program = entry.toObject();
        if (program.value(QStringLiteral("function")).toString() == function &&
            program.value(QStringLiteral("graph_resources")) == QJsonValue(false))
        {
            description += QStringLiteral(" Also enables named access for this callback in the shader manifest.");
            break;
        }
    }
    confirm.setInformativeText(description + QStringLiteral(" Save publishes the change."));
    QPushButton* apply = confirm.addButton(QStringLiteral("Add Resource Access"), QMessageBox::AcceptRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != apply)
    {
        return;
    }
    if (revision != mDocument.GetRevision() || file != GetSourceFile())
    {
        mSourceStatus->setText(QStringLiteral("The shader draft changed. Review the current source and try again."));
        return;
    }
    const RenderGraphDocumentStatus status = mDocument.AddShaderBufferAccess(manifest, file, function, error);
    documentEdited(status);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mSourceStatus->setText(error);
    }
}

void RendererWorkspace::populateBufferSizing(const QJsonObject& node, QFormLayout& outFields, QWidget& inoutParent)
{
    const QJsonObject record = node.value(QStringLiteral("shader_struct")).toObject();
    if (record.isEmpty() == false)
    {
        const QString source = record.value(QStringLiteral("source")).toString();
        const QString type = record.value(QStringLiteral("type")).toString();
        const foundation::int64 count = record.value(QStringLiteral("count")).toInteger();
        const QString heading = QStringLiteral("%L1 × %2\n%3").arg(count).arg(type, source);
        QStringList toolArguments;
        appendShaderToolArguments(toolArguments);
        BufferLayoutPreview* sizing =
            new BufferLayoutPreview(*this, mDocument, mTooling, manifestFile(), toolArguments, heading, inoutParent);
        sizing->setObjectName(QStringLiteral("renderGraphBufferStructLayout"));
        sizing->SetCount(count);
        sizing->SetRecord(source, type);
        connect(this, &RendererWorkspace::DocumentChanged, sizing, &BufferLayoutPreview::RefreshDraft);
        outFields.addRow(QStringLiteral("Shader struct"), sizing);
    }
    QPushButton* edit = new QPushButton(record.isEmpty() == true ? QStringLiteral("From shader struct…")
                                                                 : QStringLiteral("Edit buffer sizing…"),
                                        &inoutParent);
    edit->setObjectName(QStringLiteral("renderGraphEditBufferSizing"));
    edit->setProperty("renderGraphField", QStringLiteral("shader_struct"));
    QWidget* parent = &inoutParent;
    connect(edit, &QPushButton::clicked, this, [this, parent]() { editBufferSizing(*parent); });
    outFields.addRow(edit);
}

void RendererWorkspace::editBufferSizing(QWidget& inoutParent, const BufferSizingDraft* pendingOrNull)
{
    const QJsonObject node = selectedNode();
    const QString nodeFile = mSelectedFile;
    const QString configuration = mSelectedConfiguration;
    const QString nodeId = mSelectedNode;
    if (pendingOrNull == nullptr && mPendingBufferSizing.File == nodeFile &&
        mPendingBufferSizing.Configuration == configuration && mPendingBufferSizing.Node == nodeId &&
        nodeId.isEmpty() == false)
    {
        const BufferSizingDraft pending = mPendingBufferSizing;
        clearBufferSizingReturn();
        editBufferSizing(inoutParent, &pending);
        return;
    }
    const QJsonObject record = node.value(QStringLiteral("shader_struct")).toObject();
    QDialog dialog(&inoutParent);
    dialog.setWindowTitle(QStringLiteral("Buffer sizing"));
    QFormLayout fields(&dialog);
    QComboBox* mode = new QComboBox(&dialog);
    mode->setObjectName(QStringLiteral("renderGraphBufferSizingMode"));
    mode->addItems({QStringLiteral("Fixed bytes"),
                    QStringLiteral("From shader struct"),
                    QStringLiteral("Native structured buffer")});
    const bool isNativeStructured = record.value(QStringLiteral("access")).toString() == QStringLiteral("Structured");
    mode->setCurrentIndex(pendingOrNull != nullptr ? pendingOrNull->Mode : isNativeStructured == true ? 2 : 1);
    fields.addRow(QStringLiteral("Sizing"), mode);

    QComboBox* source = new QComboBox(&dialog);
    source->setObjectName(QStringLiteral("renderGraphBufferStructSource"));
    for (const QString& file : mDocument.GetFiles())
    {
        if (file.endsWith(GetShaderIncludeSuffix(RenderGraphShaderLanguage::Slang)) == true ||
            file.endsWith(GetShaderIncludeSuffix(RenderGraphShaderLanguage::Lusl)) == true)
        {
            source->addItem(file);
        }
    }
    const QString previousSource = record.value(QStringLiteral("source")).toString();
    source->setPlaceholderText(QStringLiteral("Select a shader declaration file"));
    source->setCurrentIndex(source->findText(pendingOrNull == nullptr ? previousSource : pendingOrNull->Source));
    fields.addRow(QStringLiteral("Shader declaration file"), source);
    QLabel* alignmentNote = new QLabel(BUFFER_LAYOUT_ALIGNMENT_NOTE, &dialog);
    alignmentNote->setObjectName(QStringLiteral("renderGraphBufferLayoutAlignmentNote"));
    alignmentNote->setWordWrap(true);
    QWidget* declarationActions = new QWidget(&dialog);
    QHBoxLayout* declarationLayout = new QHBoxLayout(declarationActions);
    declarationLayout->setContentsMargins(0, 0, 0, 0);
    QPushButton* edit = new QPushButton(QStringLiteral("Edit Declaration…"), declarationActions);
    edit->setObjectName(QStringLiteral("renderGraphEditBufferDeclaration"));
    edit->setAutoDefault(false);
    declarationLayout->addWidget(edit);
    QPushButton* create = new QPushButton(QStringLiteral("New Shared Shader File…"), declarationActions);
    create->setObjectName(QStringLiteral("renderGraphNewSharedShader"));
    create->setAutoDefault(false);
    declarationLayout->addWidget(create);
    declarationLayout->addStretch();
    fields.addRow(declarationActions);

    QComboBox* type = new QComboBox(&dialog);
    type->setObjectName(QStringLiteral("renderGraphBufferStructType"));
    type->setPlaceholderText(QStringLiteral("Select a declared struct"));
    fields.addRow(QStringLiteral("Declared struct"), type);
    QSpinBox* count = new QSpinBox(&dialog);
    count->setObjectName(QStringLiteral("renderGraphBufferStructCount"));
    count->setRange(1, format::MAX_RAY_BUFFER_BYTES);
    count->setValue(pendingOrNull == nullptr ? record.value(QStringLiteral("count")).toInt(1) : pendingOrNull->Count);
    fields.addRow(QStringLiteral("Element capacity"), count);
    QStringList toolArguments;
    appendShaderToolArguments(toolArguments);
    BufferLayoutPreview* sizing =
        new BufferLayoutPreview(*this, mDocument, mTooling, manifestFile(), toolArguments, {}, dialog);
    sizing->setObjectName(QStringLiteral("renderGraphBufferBytePreview"));
    sizing->SetCount(count->value());
    fields.addRow(QStringLiteral("Storage size"), sizing);
    connect(count, &QSpinBox::valueChanged, sizing, &BufferLayoutPreview::SetCount);
    connect(this, &RendererWorkspace::DocumentChanged, sizing, &BufferLayoutPreview::RefreshDraft);
    QSpinBox* bytes = new QSpinBox(&dialog);
    bytes->setObjectName(QStringLiteral("renderGraphFixedBufferBytes"));
    bytes->setRange(format::RAY_BUFFER_WORD_BYTES, format::MAX_RAY_BUFFER_BYTES);
    bytes->setSingleStep(format::RAY_BUFFER_WORD_BYTES);
    bytes->setValue(pendingOrNull == nullptr ? node.value(QStringLiteral("bytes")).toInt(format::RAY_BUFFER_WORD_BYTES)
                                             : pendingOrNull->Bytes);
    fields.addRow(QStringLiteral("Capacity (bytes)"), bytes);
    fields.addRow(alignmentNote);

    QLabel* hint = new QLabel(&dialog);
    hint->setObjectName(QStringLiteral("renderGraphBufferSizingHint"));
    hint->setTextFormat(Qt::PlainText);
    hint->setWordWrap(true);
    fields.addRow(hint);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    fields.addRow(buttons);
    QPushButton* apply = buttons->button(QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto refresh =
        [&fields, mode, source, type, declarationActions, edit, alignmentNote, count, sizing, bytes, hint, apply]() {
            const bool isFromStruct = mode->currentIndex() != 0;
            fields.setRowVisible(source, isFromStruct);
            fields.setRowVisible(declarationActions, isFromStruct);
            edit->setEnabled(source->currentIndex() >= 0);
            fields.setRowVisible(alignmentNote, isFromStruct);
            fields.setRowVisible(type, isFromStruct);
            fields.setRowVisible(count, isFromStruct);
            const bool hasRecord = isFromStruct == true && type->currentIndex() >= 0;
            fields.setRowVisible(sizing, hasRecord);
            sizing->SetRecord(source->currentText(), hasRecord == true ? type->currentText() : QString());
            fields.setRowVisible(bytes, isFromStruct == false);
            QString message;
            if (isFromStruct == false)
            {
                message = QStringLiteral("Enter a capacity in whole 16-byte buffer words.");
            }
            else if (source->currentIndex() < 0)
            {
                message = QStringLiteral("Choose a declaration file or create a shared shader file.");
            }
            else if (type->count() == 0)
            {
                message = QStringLiteral("No struct declaration was found in '%1'. Use Edit Declaration to add one.")
                              .arg(source->currentText());
            }
            else if (type->currentIndex() < 0)
            {
                message = QStringLiteral("Choose the struct that describes one buffer element.");
            }
            else if (mode->currentIndex() == 2)
            {
                message = QStringLiteral("Use named Buffer access in a scene compute writer and a Metal hardware ray "
                                         "reader. Open Shader API for the native resource type.");
            }
            else
            {
                message = QStringLiteral("Open Shader API on a connected shader node to see the named resource.");
            }
            hint->setText(message);
            apply->setEnabled(isFromStruct == true ? type->currentIndex() >= 0
                                                   : bytes->value() % format::RAY_BUFFER_WORD_BYTES == 0);
        };
    const auto listStructs = [this, source, type, refresh](const QString& preferredType) {
        const QSignalBlocker blocker(type);
        type->clear();
        const QString file = source->currentText();
        type->addItems(FindDeclaredStructs(mDocument.GetDraft(file)));
        const foundation::int32 index = type->findText(preferredType);
        type->setCurrentIndex(index < 0 && type->count() == 1 ? 0 : index);
        refresh();
    };
    const auto selectSource = [source, record, previousSource, listStructs]() {
        listStructs(source->currentText() == previousSource ? record.value(QStringLiteral("type")).toString()
                                                            : QString());
    };
    connect(mode, &QComboBox::currentIndexChanged, &dialog, refresh);
    connect(source, &QComboBox::currentIndexChanged, &dialog, selectSource);
    connect(type, &QComboBox::currentIndexChanged, &dialog, refresh);
    connect(bytes, &QSpinBox::valueChanged, &dialog, refresh);

    connect(edit,
            &QPushButton::clicked,
            &dialog,
            [this, &dialog, &inoutParent, nodeFile, configuration, nodeId, mode, source, type, count, bytes, hint]() {
                const QString file = source->currentText();
                if (mShaderCodeArea.isNull() == true || mDocument.GetFiles().contains(file) == false)
                {
                    hint->setText(
                        QStringLiteral("Could not open the declaration. Choose an available package source."));
                    return;
                }
                if (mPendingBufferSizing.Node.isEmpty() == false)
                {
                    hint->setText(QStringLiteral(
                        "Return to the pending Buffer sizing from the Code tab before editing another declaration."));
                    return;
                }
                count->interpretText();
                bytes->interpretText();
                mPendingBufferSizing =
                {
                    .File = nodeFile,
                    .Configuration = configuration,
                    .Node = nodeId,
                    .Source = file,
                    .Type = type->currentText(),
                    .Mode = mode->currentIndex(),
                    .Count = count->value(),
                    .Bytes = bytes->value(),
                };
                mShaderCodeArea->SetSessionAction(QStringLiteral("Return to Buffer Sizing"));
                dialog.reject();
                RenderGraphNodeDetails* detailsOrNull = qobject_cast<RenderGraphNodeDetails*>(inoutParent.window());
                if (detailsOrNull != nullptr)
                {
                    detailsOrNull->accept();
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, file]() {
                        if (mPendingBufferSizing.Source == file)
                        {
                            static_cast<void>(openShaderCodeTab(file, false));
                        }
                    },
                    Qt::QueuedConnection);
            });
    connect(create, &QPushButton::clicked, &dialog, [this, &dialog, source, previousSource]() {
        const QString selected = source->currentText();
        const QString nearbyFile = selected.isEmpty() == false ? selected : previousSource;
        const QString createdFile =
            stageSharedShaderSource(dialog,
                                    nearbyFile.isEmpty() == false ? nearbyFile : GetSourceFile(),
                                    SharedShaderFilePurpose::BufferLayout);
        if (createdFile.isEmpty() == false)
        {
            source->addItem(createdFile);
            source->setCurrentText(createdFile);
        }
    });
    const foundation::uint64 revision = mDocument.GetRevision();
    connect(apply,
            &QPushButton::clicked,
            &dialog,
            [this, node, nodeFile, configuration, mode, source, type, count, bytes, hint, &dialog]() {
                QJsonObject candidate = node;
                if (mode->currentIndex() != 0)
                {
                    candidate.remove(QStringLiteral("bytes"));
                    QJsonObject shaderStruct{{QStringLiteral("source"), source->currentText()},
                                             {QStringLiteral("type"), type->currentText()},
                                             {QStringLiteral("count"), count->value()}};
                    if (mode->currentIndex() == 2)
                    {
                        shaderStruct.insert(QStringLiteral("access"), QStringLiteral("Structured"));
                    }
                    candidate.insert(QStringLiteral("shader_struct"), shaderStruct);
                }
                else
                {
                    candidate.remove(QStringLiteral("shader_struct"));
                    candidate.insert(QStringLiteral("bytes"), bytes->value());
                }
                QString error;
                if (candidate == node ||
                    mDocument.ReplaceNode(nodeFile, configuration, candidate, &error) == RenderGraphDocumentStatus::Ok)
                {
                    dialog.accept();
                    return;
                }
                hint->setText(error.isEmpty() == false ? error : QStringLiteral("Reselect the Buffer and try again."));
            });
    listStructs(pendingOrNull == nullptr ? record.value(QStringLiteral("type")).toString() : pendingOrNull->Type);
    dialog.exec();
    if (mDocument.GetRevision() != revision)
    {
        documentEdited(RenderGraphDocumentStatus::Ok);
    }
}
void RendererWorkspace::clearBufferSizingReturn()
{
    mPendingBufferSizing = {};
    if (mShaderCodeArea.isNull() == false)
    {
        mShaderCodeArea->SetSessionAction({});
    }
}

void RendererWorkspace::returnToBufferSizing()
{
    const BufferSizingDraft pending = mPendingBufferSizing;
    if (pending.Node.isEmpty() == true)
    {
        return;
    }
    clearBufferSizingReturn();
    Q_EMIT GraphWorkspaceRequested();
    if (SelectNode(pending.File, pending.Configuration, pending.Node) == false ||
        selectedNode().value(QStringLiteral("kind")).toString() != QStringLiteral("Buffer"))
    {
        const QString message =
            QStringLiteral("The original Buffer is unavailable. Select a Buffer and reopen sizing.");
        mStatus->setText(message);
        Q_EMIT NodeOperationFailed(message);
        return;
    }
    editBufferSizing(*this, &pending);
}
} // namespace ludus::editor
