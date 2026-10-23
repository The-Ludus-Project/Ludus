#include "internal/error_list.h"
#include "internal/item_selection.h"

#include <ludus/foundation/base/core.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
enum Column : foundation::uint8
{
    SEVERITY_COLUMN,
    CODE_COLUMN,
    DESCRIPTION_COLUMN,
    FILE_COLUMN,
    LOCATION_COLUMN,
    ORIGIN_COLUMN,
    COLUMN_COUNT,
};
constexpr foundation::int32 ENTRY_ROLE = Qt::UserRole;
// Output lines longer than this are not compiler diagnostics worth matching.
constexpr qsizetype MAX_DIAGNOSTIC_LINE_CHARACTERS = 4096;
constexpr foundation::int32 MAX_DETAILS_HEIGHT = 100;
constexpr foundation::int32 MINIMUM_TABLE_ROWS = 2;
// Horizontal space a table cell keeps around its text; wider descriptions are elided.
constexpr foundation::int32 DESCRIPTION_CELL_PADDING_PIXELS = 8;

[[nodiscard]] bool AreEntriesEqual(const ErrorListEntry& left, const ErrorListEntry& right)
{
    return left.Severity == right.Severity && left.Producer == right.Producer && left.Code == right.Code &&
           left.Description == right.Description && left.File == right.File && left.Location == right.Location &&
           left.Origin == right.Origin && left.SourceIndex == right.SourceIndex;
}

[[nodiscard]] ErrorListSeverity ParseSeverity(const QString& text)
{
    if (text.compare(QLatin1StringView("error"), Qt::CaseInsensitive) == 0 ||
        text.compare(QLatin1StringView("fatal error"), Qt::CaseInsensitive) == 0)
    {
        return ErrorListSeverity::Error;
    }
    if (text.compare(QLatin1StringView("warning"), Qt::CaseInsensitive) == 0)
    {
        return ErrorListSeverity::Warning;
    }
    return ErrorListSeverity::Message;
}

/// Location column text for a render-graph location object: a line or a configuration/node path.
[[nodiscard]] QString DescribeRenderGraphLocation(const QJsonObject& location)
{
    QStringList parts;
    if (location.contains(QStringLiteral("line")) == true)
    {
        parts.append(QStringLiteral("line %1").arg(location.value(QStringLiteral("line")).toInt()));
    }
    if (location.contains(QStringLiteral("generated")) == true)
    {
        parts.append(
            QStringLiteral("generated wrapper of %1").arg(location.value(QStringLiteral("program")).toString()));
    }
    const QString configuration = location.value(QStringLiteral("configuration")).toString();
    const QString node = location.value(QStringLiteral("node")).toString();
    if (configuration.isEmpty() == false || node.isEmpty() == false)
    {
        parts.append(node.isEmpty() == true ? QStringLiteral("#%1").arg(configuration)
                                            : QStringLiteral("#%1/%2").arg(configuration, node));
    }
    for (const QString& key : {QStringLiteral("port"), QStringLiteral("field")})
    {
        const QString value = location.value(key).toString();
        if (value.isEmpty() == false)
        {
            parts.append(QStringLiteral("%1 %2").arg(key, value));
        }
    }
    return parts.join(QStringLiteral(", "));
}

[[nodiscard]] QString GetSeverityName(ErrorListSeverity severity)
{
    switch (severity)
    {
        case ErrorListSeverity::Error:
            return QStringLiteral("Error");
        case ErrorListSeverity::Warning:
            return QStringLiteral("Warning");
        case ErrorListSeverity::Message:
            break;
    }
    return QStringLiteral("Message");
}
} // namespace

QList<ErrorListEntry> MakeRenderGraphErrorEntries(const RenderGraphJobResult& result)
{
    // Row order mirrors RendererWorkspace::AcceptResult so SourceIndex can be passed to
    // RendererWorkspace::ActivateDiagnostic, which owns the authored-location mapping.
    QList<ErrorListEntry> entries;
    if (result.IsSuccessful == true)
    {
        entries.append(
        {
            .Severity = ErrorListSeverity::Message,
            .Producer = ErrorListProducer::RenderGraph,
            .Code = {},
            .Description = QStringLiteral("%1 succeeded for draft revision %2.")
                               .arg(result.Request.Action)
                               .arg(result.Request.Revision),
            .File = {},
            .Location = {},
            .Origin = QStringLiteral("Render graph"),
            .SourceIndex = 0,
        });
        return entries;
    }
    for (foundation::int32 row = 0; row < result.GetDiagnosticCount(); ++row)
    {
        const QJsonObject diagnostic = result.GetDiagnostic(row);
        const QJsonObject location = diagnostic.value(QStringLiteral("location")).toObject();
        const QString program = diagnostic.value(QStringLiteral("program")).toString();
        const QString target = diagnostic.value(QStringLiteral("target")).toString();
        QString description = DescribeRenderGraphDiagnostic(diagnostic);
        if (program.isEmpty() == false)
        {
            description = QStringLiteral("%1 (%2): %3").arg(program, target, description);
        }
        entries.append(
        {
            .Severity = ParseSeverity(diagnostic.value(QStringLiteral("severity")).toString()),
            .Producer = ErrorListProducer::RenderGraph,
            .Code = diagnostic.value(QStringLiteral("code")).toString(),
            .Description = description,
            .File = location.contains(QStringLiteral("generated")) == true
                        ? location.value(QStringLiteral("generated")).toString()
                        : location.value(QStringLiteral("source")).toString(),
            .Location = DescribeRenderGraphLocation(location),
            .Origin =
                program.isEmpty() == false || diagnostic.value(QStringLiteral("code"))
                                                      .toString()
                                                      .startsWith(QLatin1StringView("RenderGraphBuildShader")) == true
                    ? QStringLiteral("Shader")
                    : QStringLiteral("Render graph"),
            .SourceIndex = row,
        });
    }
    return entries;
}

QList<ErrorListEntry> ParseBuildErrorEntries(const QString& output)
{
    // Clang/GCC: `path:line[:column]: error|warning|note: message`.
    static const QRegularExpression COMPILER_DIAGNOSTIC(QStringLiteral(
        R"(^(?<file>[^:\s][^:]*?):(?<line>\d+)(?::\d+)?:\s*(?<severity>fatal error|error|warning|note):\s*(?<message>.*)$)"));
    // CMake: `CMake Error at path:line (command):`; the message follows on later lines.
    static const QRegularExpression CMAKE_DIAGNOSTIC(
        QStringLiteral(R"(^CMake (?<severity>Error|Warning)(?: \(dev\))? at (?<file>.+?):(?<line>\d+))"));
    QList<ErrorListEntry> entries;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (qsizetype index = 0; index < lines.size() && entries.size() < MAX_BUILD_DIAGNOSTICS; ++index)
    {
        const QString& line = lines.at(index);
        if (line.size() > MAX_DIAGNOSTIC_LINE_CHARACTERS)
        {
            continue;
        }
        QRegularExpressionMatch match = COMPILER_DIAGNOSTIC.match(line);
        if (match.hasMatch() == false)
        {
            match = CMAKE_DIAGNOSTIC.match(line);
        }
        if (match.hasMatch() == false)
        {
            continue;
        }
        QString message = match.captured(QStringLiteral("message"));
        if (message.isEmpty() == true && index + 1 < lines.size())
        {
            message = lines.at(index + 1).trimmed();
        }
        entries.append(
        {
            .Severity = ParseSeverity(match.captured(QStringLiteral("severity"))),
            .Producer = ErrorListProducer::Build,
            .Code = {},
            .Description = message,
            .File = match.captured(QStringLiteral("file")),
            .Location = QStringLiteral("line %1").arg(match.captured(QStringLiteral("line"))),
            .Origin = QStringLiteral("Build"),
            .SourceIndex = static_cast<foundation::int32>(index),
        });
    }
    return entries;
}

ErrorList::ErrorList(QWidget* parentOrNull)
    : QWidget(parentOrNull), mErrors(new QToolButton(this)), mWarnings(new QToolButton(this)),
      mMessages(new QToolButton(this)), mTable(new QTableWidget(this))
{
    setObjectName(QStringLiteral("errorList"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // Compact filter row, so the table keeps several rows in a short bottom dock.
    QHBoxLayout* filters = new QHBoxLayout;
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(0);
    const QStyle* style = QApplication::style();
    mErrors->setObjectName(QStringLiteral("errorListErrors"));
    mErrors->setIcon(style->standardIcon(QStyle::SP_MessageBoxCritical));
    mWarnings->setObjectName(QStringLiteral("errorListWarnings"));
    mWarnings->setIcon(style->standardIcon(QStyle::SP_MessageBoxWarning));
    mMessages->setObjectName(QStringLiteral("errorListMessages"));
    mMessages->setIcon(style->standardIcon(QStyle::SP_MessageBoxInformation));
    for (QToolButton* filter : {mErrors, mWarnings, mMessages})
    {
        filter->setCheckable(true);
        filter->setChecked(true);
        filter->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        filter->setAutoRaise(true);
        filters->addWidget(filter);
        connect(filter, &QToolButton::toggled, this, &ErrorList::render);
    }
    filters->addStretch();
    layout->addLayout(filters);
    mTable->setObjectName(QStringLiteral("errorListTable"));
    mTable->setAccessibleName(QStringLiteral("Diagnostics"));
    mTable->setColumnCount(COLUMN_COUNT);
    mTable->setHorizontalHeaderLabels({QString(),
                                       QStringLiteral("Code"),
                                       QStringLiteral("Description"),
                                       QStringLiteral("File"),
                                       QStringLiteral("Location"),
                                       QStringLiteral("Origin")});
    mTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    mTable->horizontalHeader()->setSectionResizeMode(DESCRIPTION_COLUMN, QHeaderView::Stretch);
    mTable->verticalHeader()->hide();
    mTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ConfigureItemSelection(*mTable);
    mTable->setWordWrap(false);
    mTable->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(mTable, 1);
    mDetails = new QPlainTextEdit(this);
    mDetails->setObjectName(QStringLiteral("errorListDetails"));
    mDetails->setReadOnly(true);
    mDetails->hide();
    mDetails->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mDetails->setPlaceholderText(QStringLiteral("Select a diagnostic to read its details and suggested repair."));
    layout->addWidget(mDetails);
    connect(mTable, &QTableWidget::currentCellChanged, this, [this](foundation::int32 row) {
        const QTableWidgetItem* itemOrNull = row >= 0 ? mTable->item(row, SEVERITY_COLUMN) : nullptr;
        const foundation::int32 index = itemOrNull == nullptr ? -1 : itemOrNull->data(ENTRY_ROLE).toInt();
        mDetails->setPlainText(index >= 0 && index < mEntries.size() ? mEntries.at(index).Description : QString());
        updateDetailsHeight();
    });
    connect(mTable->horizontalHeader(), &QHeaderView::sectionResized, this, &ErrorList::updateDetailsHeight);
    connect(mTable, &QTableWidget::itemActivated, this, [this](QTableWidgetItem* item) {
        static_cast<void>(ActivateVisibleRow(item->row()));
    });
    render();
}

void ErrorList::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateDetailsHeight();
}

void ErrorList::updateDetailsHeight()
{
    const foundation::int32 reservedHeight =
        mErrors->sizeHint().height() + mTable->horizontalHeader()->sizeHint().height() +
        MINIMUM_TABLE_ROWS * mTable->verticalHeader()->defaultSectionSize() + mTable->frameWidth() * 2 +
        (mTable->horizontalScrollBar()->isVisible() == true ? mTable->horizontalScrollBar()->sizeHint().height() : 0);
    const foundation::int32 availableHeight = height() - reservedHeight;
    const foundation::int32 detailsHeight =
        availableHeight < 0 ? 0 : (availableHeight > MAX_DETAILS_HEIGHT ? MAX_DETAILS_HEIGHT : availableHeight);
    mDetails->setMaximumHeight(detailsHeight);
    // A description that fits its row would only be shown twice.
    const QString description = mDetails->toPlainText();
    const bool isDescriptionTruncated =
        description.contains(QLatin1Char('\n')) == true ||
        mTable->fontMetrics().horizontalAdvance(description) + DESCRIPTION_CELL_PADDING_PIXELS >
            mTable->horizontalHeader()->sectionSize(DESCRIPTION_COLUMN);
    mDetails->setVisible(mTable->currentRow() >= 0 && detailsHeight > 0 && isDescriptionTruncated == true);
}

void ErrorList::ReplaceEntries(ErrorListProducer producer, const QList<ErrorListEntry>& entries)
{
    QList<ErrorListEntry> kept;
    QList<ErrorListEntry> previous;
    for (const ErrorListEntry& entry : mEntries)
    {
        if (entry.Producer != producer)
        {
            kept.append(entry);
        }
        else
        {
            previous.append(entry);
        }
    }
    bool isChanged = previous.size() != entries.size();
    bool hasProblem = false;
    for (qsizetype index = 0; index < entries.size(); ++index)
    {
        const ErrorListEntry& entry = entries.at(index);
        isChanged = isChanged == true || AreEntriesEqual(entry, previous.at(index)) == false;
        hasProblem = hasProblem == true || entry.Severity != ErrorListSeverity::Message;
    }
    kept.append(entries);
    mEntries = foundation::Move(kept);
    render();
    if (isChanged == true && hasProblem == true)
    {
        Q_EMIT ProblemsReported();
    }
}

foundation::int32 ErrorList::GetCount(ErrorListSeverity severity) const
{
    foundation::int32 count = 0;
    for (const ErrorListEntry& entry : mEntries)
    {
        count += entry.Severity == severity ? 1 : 0;
    }
    return count;
}

foundation::int32 ErrorList::GetVisibleCount() const
{
    return mTable->rowCount();
}

bool ErrorList::isShown(ErrorListSeverity severity) const
{
    switch (severity)
    {
        case ErrorListSeverity::Error:
            return mErrors->isChecked();
        case ErrorListSeverity::Warning:
            return mWarnings->isChecked();
        case ErrorListSeverity::Message:
            break;
    }
    return mMessages->isChecked();
}

bool ErrorList::ActivateVisibleRow(foundation::int32 row)
{
    const QTableWidgetItem* itemOrNull = mTable->item(row, SEVERITY_COLUMN);
    if (itemOrNull == nullptr)
    {
        return false;
    }
    const foundation::int32 index = itemOrNull->data(ENTRY_ROLE).toInt();
    mTable->selectRow(row);
    Q_EMIT EntryActivated(mEntries.at(index));
    return true;
}

void ErrorList::render()
{
    mErrors->setText(QStringLiteral("%1 Errors").arg(GetCount(ErrorListSeverity::Error)));
    mWarnings->setText(QStringLiteral("%1 Warnings").arg(GetCount(ErrorListSeverity::Warning)));
    mMessages->setText(QStringLiteral("%1 Messages").arg(GetCount(ErrorListSeverity::Message)));
    mTable->setRowCount(0);
    const QStyle* style = QApplication::style();
    for (qsizetype index = 0; index < mEntries.size(); ++index)
    {
        const ErrorListEntry& entry = mEntries.at(index);
        if (isShown(entry.Severity) == false)
        {
            continue;
        }
        const foundation::int32 row = mTable->rowCount();
        mTable->insertRow(row);
        QTableWidgetItem* severity = new QTableWidgetItem(GetSeverityName(entry.Severity));
        severity->setIcon(style->standardIcon(entry.Severity == ErrorListSeverity::Error ? QStyle::SP_MessageBoxCritical
                                              : entry.Severity == ErrorListSeverity::Warning
                                                  ? QStyle::SP_MessageBoxWarning
                                                  : QStyle::SP_MessageBoxInformation));
        severity->setData(ENTRY_ROLE, static_cast<foundation::int32>(index));
        mTable->setItem(row, SEVERITY_COLUMN, severity);
        mTable->setItem(row, CODE_COLUMN, new QTableWidgetItem(entry.Code));
        QTableWidgetItem* description = new QTableWidgetItem(entry.Description);
        description->setToolTip(entry.Description);
        mTable->setItem(row, DESCRIPTION_COLUMN, description);
        mTable->setItem(row, FILE_COLUMN, new QTableWidgetItem(entry.File));
        mTable->setItem(row, LOCATION_COLUMN, new QTableWidgetItem(entry.Location));
        mTable->setItem(row, ORIGIN_COLUMN, new QTableWidgetItem(entry.Origin));
    }
    if (mTable->rowCount() > 0)
    {
        mTable->setCurrentCell(0, SEVERITY_COLUMN);
    }
}
} // namespace ludus::editor
