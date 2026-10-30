#include "internal/error_list.h"
#include "internal/editor_icons.h"
#include "internal/item_selection.h"

#include <ludus/foundation/base/core.h>

#include <QAbstractItemView>
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

[[nodiscard]] bool AreEntriesEqual(const ErrorListEntry& left, const ErrorListEntry& right)
{
    return left.Severity == right.Severity && left.Producer == right.Producer && left.Code == right.Code &&
           left.Description == right.Description && left.File == right.File && left.Location == right.Location &&
           left.Origin == right.Origin && left.SourceIndex == right.SourceIndex && left.Details == right.Details &&
           left.Backends == right.Backends && left.Count == right.Count;
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

/// Compact position text: `line:column`, or a bare line when the column is unknown.
[[nodiscard]] QString DescribePosition(foundation::int32 line, foundation::int32 column)
{
    return column > 0 ? QStringLiteral("%1:%2").arg(line).arg(column) : QString::number(line);
}

/// Location column text for a render-graph location object: `line:column`, or a
/// configuration/node path.
[[nodiscard]] QString DescribeRenderGraphLocation(const QJsonObject& location)
{
    QStringList parts;
    if (location.contains(QStringLiteral("line")) == true)
    {
        parts.append(DescribePosition(location.value(QStringLiteral("line")).toInt(),
                                      location.value(QStringLiteral("column")).toInt()));
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

/// The row text of a description: its lines on one line, so a repair hint stays readable in the row.
[[nodiscard]] QString DescribeRow(const QString& description)
{
    QStringList lines = description.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString& line : lines)
    {
        line = line.trimmed();
    }
    return lines.join(QStringLiteral(" \u2014 "));
}

/// Full text for the collapsed Details pane; empty when the row says everything it knows.
[[nodiscard]] QString DescribeDetails(const ErrorListEntry& entry)
{
    if (entry.Details.isEmpty() == false)
    {
        return entry.Description + QStringLiteral("\n\n") + entry.Details;
    }
    return entry.Description.contains(QLatin1Char('\n')) == true ? entry.Description : QString();
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

[[nodiscard]] EditorIcon GetSeverityIcon(ErrorListSeverity severity)
{
    switch (severity)
    {
        case ErrorListSeverity::Error:
            return EditorIcon::Error;
        case ErrorListSeverity::Warning:
            return EditorIcon::Warning;
        case ErrorListSeverity::Message:
            break;
    }
    return EditorIcon::Information;
}
} // namespace

QList<ErrorListEntry> MakeRenderPreviewErrorEntries(const QString& problem, const QString& file)
{
    if (problem.isEmpty() == true)
    {
        return {};
    }
    return {
    {
        .Severity = ErrorListSeverity::Error,
        .Producer = ErrorListProducer::RenderPreview,
        .Code = QStringLiteral("RenderGraphPreviewFailed"),
        .Description = problem,
        .File = file,
        .Location = {},
        .Origin = QStringLiteral("Renderer preview"),
        .SourceIndex = 0,
        .Details = {},
        .Backends = {},
        .Count = 1,
    }};
}

QList<ErrorListEntry> MakeRenderGraphErrorEntries(const RenderGraphJobResult& result)
{
    // SourceIndex stays the RendererWorkspace diagnostic row (the first one a merged row stands
    // for), so it can be passed to RendererWorkspace::ActivateDiagnostic, which owns the
    // authored-location mapping.
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
            .Details = {},
            .Backends = {},
            .Count = 1,
        });
        return entries;
    }
    const foundation::int32 diagnosticCount = result.GetDiagnosticCount();
    // Row zero is the job's own summary unless the tool reported the whole batch itself.
    const bool hasSummaryRow = diagnosticCount > result.Diagnostics.size();
    // A shader failure's summary is the compiler's raw log, which the mapped diagnostics already
    // say in one line each. It is kept collapsed under the first of them rather than as a second row.
    const bool isSummaryFolded =
        hasSummaryRow == true && diagnosticCount > 1 && result.Code == QStringLiteral("RenderGraphBuildShader");
    QString foldedLog;
    for (foundation::int32 row = 0; row < diagnosticCount; ++row)
    {
        const QJsonObject diagnostic = result.GetDiagnostic(row);
        const QJsonObject location = diagnostic.value(QStringLiteral("location")).toObject();
        const QString program = diagnostic.value(QStringLiteral("program")).toString();
        const QString target = diagnostic.value(QStringLiteral("target")).toString();
        QString description = DescribeRenderGraphDiagnostic(diagnostic);
        if (row == 0 && isSummaryFolded == true)
        {
            foldedLog = description;
            continue;
        }
        if (program.isEmpty() == false)
        {
            description = QStringLiteral("%1: %2").arg(program, description);
        }
        ErrorListEntry entry =
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
            .Details = {},
            .Backends = target.isEmpty() == true ? QStringList{} : QStringList{target},
            .Count = 1,
        };
        // The same problem reported again, usually by another backend, is one row.
        bool isDuplicate = false;
        for (ErrorListEntry& existing : entries)
        {
            if (existing.Severity != entry.Severity || existing.Code != entry.Code ||
                existing.Description != entry.Description || existing.File != entry.File ||
                existing.Location != entry.Location || existing.Origin != entry.Origin)
            {
                continue;
            }
            ++existing.Count;
            for (const QString& backend : entry.Backends)
            {
                if (existing.Backends.contains(backend) == false)
                {
                    existing.Backends.append(backend);
                }
            }
            isDuplicate = true;
            break;
        }
        if (isDuplicate == false)
        {
            entries.append(foundation::Move(entry));
        }
    }
    if (foldedLog.isEmpty() == false && entries.isEmpty() == false)
    {
        entries.first().Details = foldedLog;
    }
    return entries;
}

QList<ErrorListEntry> ParseBuildErrorEntries(const QString& output)
{
    // Clang/GCC: `path:line[:column]: error|warning|note: message`.
    static const QRegularExpression COMPILER_DIAGNOSTIC(QStringLiteral(
        R"(^(?<file>[^:\s][^:]*?):(?<line>\d+)(?::(?<column>\d+))?:\s*(?<severity>fatal error|error|warning|note):\s*(?<message>.*)$)"));
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
            .Location = DescribePosition(match.captured(QStringLiteral("line")).toInt(),
                                         match.captured(QStringLiteral("column")).toInt()),
            .Origin = QStringLiteral("Build"),
            .SourceIndex = static_cast<foundation::int32>(index),
            .Details = {},
            .Backends = {},
            .Count = 1,
        });
    }
    return entries;
}

ErrorList::ErrorList(QWidget* parentOrNull)
    : QWidget(parentOrNull), mErrors(new QToolButton(this)), mWarnings(new QToolButton(this)),
      mMessages(new QToolButton(this)), mDetailsToggle(new QToolButton(this)), mTable(new QTableWidget(this))
{
    setObjectName(QStringLiteral("errorList"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // Compact filter row, so the table keeps several rows in a short bottom dock.
    QHBoxLayout* filters = new QHBoxLayout;
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(0);
    mErrors->setObjectName(QStringLiteral("errorListErrors"));
    mErrors->setIcon(GetEditorIcon(EditorIcon::Error));
    mWarnings->setObjectName(QStringLiteral("errorListWarnings"));
    mWarnings->setIcon(GetEditorIcon(EditorIcon::Warning));
    mMessages->setObjectName(QStringLiteral("errorListMessages"));
    mMessages->setIcon(GetEditorIcon(EditorIcon::Information));
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
    // The raw compiler log and any long text stay folded until asked for.
    mDetailsToggle->setObjectName(QStringLiteral("errorListDetailsToggle"));
    mDetailsToggle->setText(QStringLiteral("Details"));
    mDetailsToggle->setCheckable(true);
    mDetailsToggle->setAutoRaise(true);
    mDetailsToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mDetailsToggle->setToolTip(QStringLiteral("Show or hide the full text and raw tool output of the selected row."));
    mDetailsToggle->setEnabled(false);
    filters->addWidget(mDetailsToggle);
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
        mDetails->setPlainText(index >= 0 && index < mEntries.size() ? DescribeDetails(mEntries.at(index)) : QString());
        updateDetailsToggle();
        updateDetailsHeight();
    });
    connect(mDetailsToggle, &QToolButton::toggled, this, &ErrorList::updateDetailsHeight);
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

void ErrorList::updateDetailsToggle()
{
    mDetailsToggle->setEnabled(mDetails->toPlainText().isEmpty() == false);
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
    mDetails->setVisible(mTable->currentRow() >= 0 && detailsHeight > 0 && mDetailsToggle->isChecked() == true &&
                         mDetails->toPlainText().isEmpty() == false);
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
    Q_EMIT CountsChanged();
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
        severity->setIcon(GetEditorIcon(GetSeverityIcon(entry.Severity)));
        severity->setData(ENTRY_ROLE, static_cast<foundation::int32>(index));
        mTable->setItem(row, SEVERITY_COLUMN, severity);
        mTable->setItem(row, CODE_COLUMN, new QTableWidgetItem(entry.Code));
        // Identical diagnostics are one row; a leading count says how many it stands for.
        QTableWidgetItem* description =
            new QTableWidgetItem((entry.Count > 1 ? QStringLiteral("\u00d7%1  ").arg(entry.Count) : QString()) +
                                 DescribeRow(entry.Description));
        description->setToolTip(entry.Description);
        mTable->setItem(row, DESCRIPTION_COLUMN, description);
        QTableWidgetItem* file = new QTableWidgetItem(entry.File);
        file->setToolTip(entry.File);
        mTable->setItem(row, FILE_COLUMN, file);
        QTableWidgetItem* location = new QTableWidgetItem(entry.Location);
        location->setToolTip(entry.Location.contains(QLatin1Char(':')) == true
                                 ? QStringLiteral("Line and column %1").arg(entry.Location)
                                 : entry.Location);
        mTable->setItem(row, LOCATION_COLUMN, location);
        const QString origin =
            entry.Backends.isEmpty() == true
                ? entry.Origin
                : QStringLiteral("%1 \u00b7 %2").arg(entry.Origin, entry.Backends.join(QStringLiteral(", ")));
        QTableWidgetItem* originItem = new QTableWidgetItem(origin);
        originItem->setToolTip(origin);
        mTable->setItem(row, ORIGIN_COLUMN, originItem);
    }
    if (mTable->rowCount() > 0)
    {
        mTable->setCurrentCell(0, SEVERITY_COLUMN);
    }
}
} // namespace ludus::editor
