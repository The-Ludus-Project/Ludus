#include "internal/render_graph_shader_language.h"
#include "internal/renderer_workspace.h"

#include <ludus/content/render_graph_format/plan.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVariant>

namespace ludus::editor
{
namespace
{
const QString BUFFER_LAYOUT_LANGUAGE_NOTE =
    QStringLiteral("Automatic sizing uses a .slang declaration because Build Graph reflects the storage layout "
                   "directly from Slang source.");

QStringList FindDeclaredStructs(const QByteArray& source)
{
    // Recognize declarations in the selected file. Slang resolves preprocessing and
    // verifies the compiled storage layout during Build Graph.
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
        isBufferLayout == true || nearbyFile.endsWith(GetShaderSourceSuffix(RenderGraphShaderLanguage::Slang)) == true
            ? RenderGraphShaderLanguage::Slang
            : RenderGraphShaderLanguage::Lusl;
    QComboBox* language = new QComboBox(&dialog);
    language->setObjectName(QStringLiteral("renderGraphSharedShaderLanguage"));
    if (isBufferLayout == false)
    {
        language->addItem(
            QStringLiteral("LUSL include (%1)").arg(GetShaderIncludeSuffix(RenderGraphShaderLanguage::Lusl)),
            QVariant::fromValue(RenderGraphShaderLanguage::Lusl));
    }
    language->addItem(
        QStringLiteral("Slang include (%1)").arg(GetShaderIncludeSuffix(RenderGraphShaderLanguage::Slang)),
        QVariant::fromValue(RenderGraphShaderLanguage::Slang));
    language->setCurrentIndex(language->findData(QVariant::fromValue(defaultLanguage)));
    language->setEnabled(isBufferLayout == false);
    fields.addRow(QStringLiteral("Language"), language);

    const QDir directory(QFileInfo(nearbyFile).path());
    const QString defaultPath =
        QDir::cleanPath(directory.filePath(QStringLiteral("shared") + GetShaderIncludeSuffix(defaultLanguage)));
    QLineEdit* path = new QLineEdit(defaultPath, &dialog);
    path->setObjectName(QStringLiteral("renderGraphSharedShaderPath"));
    path->selectAll();
    fields.addRow(QStringLiteral("Package-relative path"), path);
    const QString usage =
        QStringLiteral("Declare shared types and helpers here. Use a quoted #include from the shaders "
                       "that need them. Save writes the file to the graph package.");
    QLabel* hint =
        new QLabel(isBufferLayout == true ? BUFFER_LAYOUT_LANGUAGE_NOTE + QLatin1Char('\n') + usage : usage, &dialog);
    hint->setObjectName(QStringLiteral("renderGraphSharedShaderHint"));
    hint->setWordWrap(true);
    fields.addRow(hint);
    QLabel* problem = new QLabel(&dialog);
    problem->setObjectName(QStringLiteral("renderGraphSharedShaderError"));
    problem->setTextFormat(Qt::PlainText);
    problem->setWordWrap(true);
    fields.addRow(problem);
    connect(path, &QLineEdit::textEdited, problem, &QLabel::clear);
    connect(language, &QComboBox::currentIndexChanged, &dialog, [language, path, problem]() {
        const RenderGraphShaderLanguage selected = language->currentData().value<RenderGraphShaderLanguage>();
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
    connect(create, &QPushButton::clicked, &dialog, [this, path, language, problem, &dialog]() {
        QString error;
        const RenderGraphShaderLanguage selected = language->currentData().value<RenderGraphShaderLanguage>();
        if (mDocument.CreateSharedShaderSource(path->text().trimmed(), selected, error) ==
            RenderGraphDocumentStatus::Ok)
        {
            dialog.accept();
            return;
        }
        problem->setText(error);
        path->setFocus();
    });
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

void RendererWorkspace::editBufferSizing(QWidget& inoutParent)
{
    namespace format = content::render_graph_format;
    const QJsonObject node = selectedNode();
    const QJsonObject record = node.value(QStringLiteral("shader_struct")).toObject();
    QDialog dialog(&inoutParent);
    dialog.setWindowTitle(QStringLiteral("Buffer sizing"));
    QFormLayout fields(&dialog);
    QComboBox* mode = new QComboBox(&dialog);
    mode->setObjectName(QStringLiteral("renderGraphBufferSizingMode"));
    mode->addItems({QStringLiteral("Fixed bytes"), QStringLiteral("From shader struct")});
    mode->setCurrentIndex(1);
    fields.addRow(QStringLiteral("Sizing"), mode);

    const QJsonArray programs = QJsonDocument::fromJson(mDocument.GetDraft(manifestFile()))
                                    .object()
                                    .value(QStringLiteral("programs"))
                                    .toArray();
    QSet<QString> programSources;
    for (const QJsonValueConstRef entry : programs)
    {
        programSources.insert(entry.toObject().value(QStringLiteral("source")).toString());
    }
    QComboBox* source = new QComboBox(&dialog);
    source->setObjectName(QStringLiteral("renderGraphBufferStructSource"));
    for (const QString& file : mDocument.GetFiles())
    {
        if (file.endsWith(GetShaderIncludeSuffix(RenderGraphShaderLanguage::Slang)) == true &&
            programSources.contains(file) == false)
        {
            source->addItem(file);
        }
    }
    const QString previousSource = record.value(QStringLiteral("source")).toString();
    source->setPlaceholderText(QStringLiteral("Select a shared Slang file"));
    source->setCurrentIndex(source->findText(previousSource));
    fields.addRow(QStringLiteral("Shared Slang file"), source);
    QLabel* languageNote = new QLabel(BUFFER_LAYOUT_LANGUAGE_NOTE, &dialog);
    languageNote->setObjectName(QStringLiteral("renderGraphBufferLayoutLanguageNote"));
    languageNote->setWordWrap(true);
    fields.addRow(languageNote);
    QPushButton* create = new QPushButton(QStringLiteral("New shared shader file…"), &dialog);
    create->setObjectName(QStringLiteral("renderGraphNewSharedShader"));
    create->setAutoDefault(false);
    fields.addRow(create);

    QComboBox* type = new QComboBox(&dialog);
    type->setObjectName(QStringLiteral("renderGraphBufferStructType"));
    type->setPlaceholderText(QStringLiteral("Select a declared struct"));
    fields.addRow(QStringLiteral("Declared struct"), type);
    QSpinBox* count = new QSpinBox(&dialog);
    count->setObjectName(QStringLiteral("renderGraphBufferStructCount"));
    count->setRange(1, format::MAX_RAY_BUFFER_BYTES);
    count->setValue(record.value(QStringLiteral("count")).toInt(1));
    fields.addRow(QStringLiteral("Element capacity"), count);
    QSpinBox* bytes = new QSpinBox(&dialog);
    bytes->setObjectName(QStringLiteral("renderGraphFixedBufferBytes"));
    bytes->setRange(format::RAY_BUFFER_WORD_BYTES, format::MAX_RAY_BUFFER_BYTES);
    bytes->setSingleStep(format::RAY_BUFFER_WORD_BYTES);
    bytes->setValue(node.value(QStringLiteral("bytes")).toInt(format::RAY_BUFFER_WORD_BYTES));
    fields.addRow(QStringLiteral("Capacity (bytes)"), bytes);

    QLabel* hint = new QLabel(&dialog);
    hint->setObjectName(QStringLiteral("renderGraphBufferSizingHint"));
    hint->setTextFormat(Qt::PlainText);
    hint->setWordWrap(true);
    fields.addRow(hint);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    fields.addRow(buttons);
    QPushButton* apply = buttons->button(QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const bool isPreviousProgram = programSources.contains(previousSource);
    const auto refresh = [&fields,
                          mode,
                          source,
                          type,
                          create,
                          languageNote,
                          count,
                          bytes,
                          hint,
                          apply,
                          previousSource,
                          isPreviousProgram]() {
        const bool isFromStruct = mode->currentIndex() == 1;
        fields.setRowVisible(source, isFromStruct);
        fields.setRowVisible(create, isFromStruct);
        fields.setRowVisible(languageNote, isFromStruct);
        fields.setRowVisible(type, isFromStruct);
        fields.setRowVisible(count, isFromStruct);
        fields.setRowVisible(bytes, isFromStruct == false);
        QString message;
        if (isFromStruct == false)
        {
            message = QStringLiteral("Enter a capacity in whole 16-byte buffer words.");
        }
        else if (source->currentIndex() < 0 && isPreviousProgram == true)
        {
            message =
                QStringLiteral("'%1' is a shader program source. Move the buffer layout into a shared .slang file, "
                               "include it from the connected shaders, and select it here.")
                    .arg(previousSource);
        }
        else if (source->currentIndex() < 0)
        {
            message = QStringLiteral("Choose or create a shared .slang file that declares the buffer layout. "
                                     "The Shaders list is for executable programs.");
        }
        else if (type->count() == 0)
        {
            message = QStringLiteral("No struct declaration was found in '%1'. Declare the struct in Source "
                                     "or select a different shared file.")
                          .arg(source->currentText());
        }
        else
        {
            message = QStringLiteral("Use a quoted #include in each connected Slang shader that uses this layout. "
                                     "Build Graph verifies the shader and calculates the allocation.");
        }
        hint->setText(message);
        apply->setEnabled(isFromStruct == true ? type->currentIndex() >= 0
                                               : bytes->value() % format::RAY_BUFFER_WORD_BYTES == 0);
    };
    const auto selectSource = [this, source, type, record, previousSource, refresh]() {
        const QSignalBlocker blocker(type);
        type->clear();
        const QString file = source->currentText();
        type->addItems(FindDeclaredStructs(mDocument.GetDraft(file)));
        const foundation::int32 index =
            file == previousSource ? type->findText(record.value(QStringLiteral("type")).toString()) : -1;
        type->setCurrentIndex(index < 0 && type->count() == 1 ? 0 : index);
        refresh();
    };
    connect(mode, &QComboBox::currentIndexChanged, &dialog, refresh);
    connect(source, &QComboBox::currentIndexChanged, &dialog, selectSource);
    connect(type, &QComboBox::currentIndexChanged, &dialog, refresh);
    connect(bytes, &QSpinBox::valueChanged, &dialog, refresh);

    QString createdFile;
    connect(create, &QPushButton::clicked, &dialog, [this, &dialog, &createdFile, source, previousSource]() {
        const QString selected = source->currentText();
        const QString nearbyFile = selected.isEmpty() == false ? selected : previousSource;
        createdFile = stageSharedShaderSource(dialog,
                                              nearbyFile.isEmpty() == false ? nearbyFile : GetSourceFile(),
                                              SharedShaderFilePurpose::BufferLayout);
        if (createdFile.isEmpty() == false)
        {
            dialog.accept();
        }
    });
    const foundation::uint64 revision = mDocument.GetRevision();
    connect(apply, &QPushButton::clicked, &dialog, [this, node, mode, source, type, count, bytes, hint, &dialog]() {
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
        if (candidate == node || mDocument.ReplaceNode(mSelectedFile, mSelectedConfiguration, candidate, &error) ==
                                     RenderGraphDocumentStatus::Ok)
        {
            dialog.accept();
            return;
        }
        hint->setText(error.isEmpty() == false ? error : QStringLiteral("Reselect the Buffer and try again."));
    });
    selectSource();
    if (dialog.exec() == QDialog::Accepted)
    {
        if (createdFile.isEmpty() == false)
        {
            QDialog* detailsOrNull = qobject_cast<QDialog*>(inoutParent.window());
            if (detailsOrNull != nullptr)
            {
                detailsOrNull->accept();
            }
        }
        if (mDocument.GetRevision() != revision)
        {
            documentEdited(RenderGraphDocumentStatus::Ok);
        }
        if (createdFile.isEmpty() == false)
        {
            openSourceLine(createdFile, 1);
        }
    }
}
} // namespace ludus::editor
