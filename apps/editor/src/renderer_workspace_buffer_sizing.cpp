#include "internal/code_editor.h"
#include "internal/code_editor_area.h"
#include "internal/render_graph_node_details.h"
#include "internal/render_graph_shader_language.h"
#include "internal/renderer_workspace.h"

#include <ludus/content/render_graph_format/plan.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVariant>

namespace ludus::editor
{
namespace
{
const QString BUFFER_LAYOUT_LANGUAGE_NOTE =
    QStringLiteral("Build Graph reflects the storage layout from the selected shader declaration.");
constexpr foundation::int32 DECLARATION_EDITOR_LINES = 12;

QStringList FindDeclaredStructs(const QByteArray& source)
{
    // The picker lists local declarations. Build Graph preprocesses the source and verifies its storage layout.
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
    const QString usage =
        isBufferLayout == true
            ? QStringLiteral("Define the buffer struct below. The file joins the graph draft when you create it.")
            : QStringLiteral("Declare shared types and helpers here. Use a quoted #include from the shaders "
                             "that need them. Save writes the file to the graph package.");
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
    connect(create, &QPushButton::clicked, &dialog, [this, path, language, problem, editorOrNull, &dialog]() {
        QByteArray source;
        if (editorOrNull != nullptr && ReadBufferDeclaration(*editorOrNull, *problem, source) == false)
        {
            return;
        }
        QString error;
        const RenderGraphShaderLanguage selected = language->currentData().value<RenderGraphShaderLanguage>();
        const QString file = path->text().trimmed();
        const RenderGraphDocumentStatus status = mDocument.CreateSharedShaderSource(file, selected, error, source);
        if (status == RenderGraphDocumentStatus::Ok)
        {
            dialog.accept();
            return;
        }
        if (editorOrNull != nullptr && status == RenderGraphDocumentStatus::Conflict &&
            mDocument.GetFiles().contains(file, Qt::CaseInsensitive) == true)
        {
            error = QStringLiteral("'%1' already exists. Choose another path, or cancel and select it in Buffer "
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

void RendererWorkspace::editBufferSizing(QWidget& inoutParent, const BufferSizingDraft* pendingOrNull)
{
    namespace format = content::render_graph_format;
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
    mode->addItems({QStringLiteral("Fixed bytes"), QStringLiteral("From shader struct")});
    mode->setCurrentIndex(pendingOrNull == nullptr ? 1 : pendingOrNull->Mode);
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
    QLabel* languageNote = new QLabel(BUFFER_LAYOUT_LANGUAGE_NOTE, &dialog);
    languageNote->setObjectName(QStringLiteral("renderGraphBufferLayoutLanguageNote"));
    languageNote->setWordWrap(true);
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
    QSpinBox* bytes = new QSpinBox(&dialog);
    bytes->setObjectName(QStringLiteral("renderGraphFixedBufferBytes"));
    bytes->setRange(format::RAY_BUFFER_WORD_BYTES, format::MAX_RAY_BUFFER_BYTES);
    bytes->setSingleStep(format::RAY_BUFFER_WORD_BYTES);
    bytes->setValue(pendingOrNull == nullptr ? node.value(QStringLiteral("bytes")).toInt(format::RAY_BUFFER_WORD_BYTES)
                                             : pendingOrNull->Bytes);
    fields.addRow(QStringLiteral("Capacity (bytes)"), bytes);
    fields.addRow(languageNote);

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
        [&fields, mode, source, type, declarationActions, edit, languageNote, count, bytes, hint, apply]() {
            const bool isFromStruct = mode->currentIndex() == 1;
            fields.setRowVisible(source, isFromStruct);
            fields.setRowVisible(declarationActions, isFromStruct);
            edit->setEnabled(source->currentIndex() >= 0);
            fields.setRowVisible(languageNote, isFromStruct);
            fields.setRowVisible(type, isFromStruct);
            fields.setRowVisible(count, isFromStruct);
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
                if (mode->currentIndex() == 1)
                {
                    candidate.remove(QStringLiteral("bytes"));
                    candidate.insert(QStringLiteral("shader_struct"),
                                     QJsonObject{{QStringLiteral("source"), source->currentText()},
                                                 {QStringLiteral("type"), type->currentText()},
                                                 {QStringLiteral("count"), count->value()}});
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
