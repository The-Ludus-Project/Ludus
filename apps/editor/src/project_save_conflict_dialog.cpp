#include "internal/project_save_conflict_dialog.h"

#include "internal/project_store.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
QString FormatValue(const QJsonValue& value)
{
    if (value.isUndefined() == true)
    {
        return QStringLiteral("(absent)");
    }
    // Qt 6.4 serializes scalar values through a document container. Keep JSON
    // escaping so an empty value or embedded newline is visible in the comparison.
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

void AppendChangedFields(QStringList& inoutRows,
                         const QString& prefix,
                         const QJsonObject& baseline,
                         const QJsonObject& disk,
                         const QJsonObject& draft,
                         bool isDiskValid)
{
    QStringList keys = baseline.keys() + disk.keys() + draft.keys();
    keys.removeDuplicates();
    keys.sort();
    for (const QString& key : keys)
    {
        const QJsonValue savedValue = baseline.value(key);
        const QJsonValue diskValue = disk.value(key);
        const QJsonValue draftValue = draft.value(key);
        const bool hasDiskChange = isDiskValid == true && diskValue != savedValue;
        const bool hasDraftChange = draftValue != savedValue;
        if (hasDiskChange == false && hasDraftChange == false)
        {
            continue;
        }
        const QString field = prefix + key;
        if (savedValue.isObject() == true || diskValue.isObject() == true || draftValue.isObject() == true)
        {
            AppendChangedFields(inoutRows,
                                field + QLatin1Char('.'),
                                savedValue.toObject(),
                                diskValue.toObject(),
                                draftValue.toObject(),
                                isDiskValid);
            continue;
        }

        QString heading = field;
        if (hasDiskChange == true && hasDraftChange == true)
        {
            heading += QStringLiteral(" (changed on disk and in draft)");
        }
        inoutRows.append(QStringLiteral("%1\n  Saved baseline  %2\n  On disk         %3\n  Pending draft   %4")
                             .arg(heading,
                                  FormatValue(savedValue),
                                  isDiskValid == true ? FormatValue(diskValue) : QStringLiteral("(unavailable)"),
                                  FormatValue(draftValue)));
    }
}

QJsonObject GetDescriptorFields(const ProjectDescriptor& descriptor)
{
    return QJsonDocument::fromJson(SerializeDescriptor(descriptor)).object();
}
} // namespace

bool ConfirmProjectConflictReload(QWidget& inoutParent,
                                  const ProjectDescriptor& baseline,
                                  const LoadOutcome& disk,
                                  const ProjectDescriptor& draft)
{
    QDialog dialog(&inoutParent);
    dialog.setObjectName(QStringLiteral("projectSaveConflict"));
    dialog.setWindowTitle(QStringLiteral("Project Settings save conflict"));
    constexpr int DIALOG_WIDTH = 620;
    constexpr int DIALOG_HEIGHT = 420;
    dialog.resize(DIALOG_WIDTH, DIALOG_HEIGHT);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QString explanation = QStringLiteral(
        "The project descriptor on disk no longer matches the saved baseline. "
        "Cancel keeps your draft and undo history. Reload validates the latest disk values before replacing your "
        "draft and clearing its undo history.");
    if (disk.Ok() == false)
    {
        explanation += QStringLiteral("\n\nDisk values are unavailable. %1\n"
                                      "Repair or restore the descriptor before reloading.")
                           .arg(disk.Parse.Message);
    }
    QLabel* summary = new QLabel(explanation, &dialog);
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(summary);

    QStringList rows;
    AppendChangedFields(rows,
                        QString(),
                        GetDescriptorFields(baseline),
                        disk.Ok() == true ? GetDescriptorFields(disk.Parse.Descriptor) : QJsonObject(),
                        GetDescriptorFields(draft),
                        disk.Ok());
    QPlainTextEdit* comparison =
        new QPlainTextEdit(rows.isEmpty() == true ? QStringLiteral("No descriptor field values changed.")
                                                  : rows.join(QStringLiteral("\n\n")),
                           &dialog);
    comparison->setObjectName(QStringLiteral("projectSaveConflictFields"));
    comparison->setAccessibleName(QStringLiteral("Changed descriptor fields"));
    comparison->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    comparison->setReadOnly(true);
    layout->addWidget(comparison);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton* reload = buttons->addButton(QStringLiteral("Reload"), QDialogButtonBox::AcceptRole);
    reload->setObjectName(QStringLiteral("projectSaveConflictReload"));
    QPushButton* cancel = buttons->button(QDialogButtonBox::Cancel);
    cancel->setDefault(true);
    cancel->setFocus();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}
} // namespace ludus::editor
