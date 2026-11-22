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

namespace ludus::editor
{
namespace
{
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

QString RendererWorkspace::stageSharedSlangSource(QWidget& inoutParent)
{
    if (mDocument.IsLoaded() == false)
    {
        return {};
    }

    QDialog dialog(&inoutParent);
    dialog.setWindowTitle(QStringLiteral("New shared Slang file"));
    QFormLayout fields(&dialog);
    const QString directory = QFileInfo(GetSourceFile()).path();
    QLineEdit* path = new QLineEdit(QDir::cleanPath(QDir(directory).filePath(QStringLiteral("shared.slang"))), &dialog);
    path->setObjectName(QStringLiteral("renderGraphSharedSlangPath"));
    path->selectAll();
    fields.addRow(QStringLiteral("Package-relative path"), path);
    QLabel* hint =
        new QLabel(QStringLiteral("Declare shared structs and helpers here. Use a quoted #include from the shaders "
                                  "that need them. Save writes the file to the graph package."),
                   &dialog);
    hint->setWordWrap(true);
    fields.addRow(hint);
    QLabel* problem = new QLabel(&dialog);
    problem->setObjectName(QStringLiteral("renderGraphSharedSlangError"));
    problem->setTextFormat(Qt::PlainText);
    problem->setWordWrap(true);
    fields.addRow(problem);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton* create = buttons->addButton(QStringLiteral("Create Shared File"), QDialogButtonBox::AcceptRole);
    create->setObjectName(QStringLiteral("renderGraphCreateSharedSlang"));
    fields.addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(create, &QPushButton::clicked, &dialog, [this, path, problem, &dialog]() {
        QString error;
        if (mDocument.CreateSharedSlangSource(path->text().trimmed(), error) == RenderGraphDocumentStatus::Ok)
        {
            dialog.accept();
            return;
        }
        problem->setText(error);
        path->setFocus();
    });
    return dialog.exec() == QDialog::Accepted ? path->text().trimmed() : QString();
}

void RendererWorkspace::openNewSharedSlangSource(QWidget& inoutParent)
{
    const QString file = stageSharedSlangSource(inoutParent);
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
        if (file.endsWith(QStringLiteral(".slang")) == true && programSources.contains(file) == false)
        {
            source->addItem(file);
        }
    }
    const QString previousSource = record.value(QStringLiteral("source")).toString();
    source->setPlaceholderText(QStringLiteral("Select a shared Slang file"));
    source->setCurrentIndex(source->findText(previousSource));
    fields.addRow(QStringLiteral("Shared Slang file"), source);
    QPushButton* create = new QPushButton(QStringLiteral("New shared Slang file…"), &dialog);
    create->setObjectName(QStringLiteral("renderGraphNewSharedSlang"));
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
    const auto refresh =
        [&fields, mode, source, type, create, count, bytes, hint, apply, previousSource, isPreviousProgram]() {
            const bool isFromStruct = mode->currentIndex() == 1;
            fields.setRowVisible(source, isFromStruct);
            fields.setRowVisible(create, isFromStruct);
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
    connect(create, &QPushButton::clicked, &dialog, [this, &dialog, &createdFile]() {
        createdFile = stageSharedSlangSource(dialog);
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
