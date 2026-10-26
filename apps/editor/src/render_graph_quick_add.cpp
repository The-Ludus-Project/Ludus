#include "internal/render_graph_quick_add.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/stable_sort.hpp>

#include <QApplication>
#include <QButtonGroup>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRectF>
#include <QRegularExpression>
#include <QRgb>
#include <QScreen>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;

namespace
{
constexpr int32 CATEGORY_WIDTH = 176;
constexpr int32 SIGNATURE_WIDTH = 168;
constexpr int32 PIN_WIDTH = 28;
constexpr int32 PALETTE_WIDTH = 720;
constexpr int32 PALETTE_MARGIN = 10;
constexpr int32 SECTION_SPACING = 8;
constexpr int32 RESULT_COLUMN_COUNT = 3;
// Section headings are smaller, slightly spaced capitals (percent of normal letter spacing).
constexpr float64 HEADING_FONT_SCALE = 0.85;
constexpr float64 HEADING_LETTER_SPACING = 108.0;
// Kind glyphs in a 16-pixel icon, matching the canvas's snippet (amber) and node (teal) accents.
constexpr QRgb SNIPPET_GLYPH_COLOR = 0xffc8842a;
constexpr QRgb NODE_GLYPH_COLOR = 0xff2a9d80;
constexpr float64 GLYPH_STROKE_WIDTH = 1.5;
constexpr int32 ICON_SIZE = 16;
// A node is one inset square; a snippet is two offset squares, back-left and front-right.
constexpr float64 NODE_GLYPH_INSET = 2.5;
constexpr float64 NODE_GLYPH_EXTENT = ICON_SIZE - 2 * NODE_GLYPH_INSET;
constexpr float64 NODE_GLYPH_RADIUS = 2.0;
constexpr float64 SNIPPET_GLYPH_NEAR = 1.5;
constexpr float64 SNIPPET_GLYPH_EXTENT = 8.0;
constexpr float64 SNIPPET_GLYPH_FAR = ICON_SIZE - SNIPPET_GLYPH_NEAR - SNIPPET_GLYPH_EXTENT - 1;
constexpr float64 SNIPPET_GLYPH_RADIUS = 1.5;
// Query scores: a label word prefix outranks a label substring, then keywords, category and description.
constexpr int32 LABEL_PREFIX_SCORE = 30;
constexpr int32 LABEL_SUBSTRING_SCORE = 18;
constexpr int32 KEYWORD_SCORE = 14;
constexpr int32 CATEGORY_SCORE = 8;
constexpr int32 DESCRIPTION_SCORE = 3;
constexpr int32 WHOLE_LABEL_PREFIX_BONUS = 40;
// Item data roles: the listed index of a result row, and a category row's identity.
constexpr int32 LISTED_INDEX_ROLE = Qt::UserRole + 1;
constexpr int32 CATEGORY_ROLE = Qt::UserRole + 2;

QString GetSuggestedCategory()
{
    return QStringLiteral("suggested");
}

QString GetAllCategory()
{
    return QStringLiteral("all");
}

/// Lowercase words of a display or identity string, splitting camel case: "ToneMap" -> tone, map.
QStringList SplitWords(const QString& text)
{
    QString spaced = text;
    spaced.replace(QRegularExpression(QStringLiteral("(?<=[a-z0-9])(?=[A-Z])")), QStringLiteral(" "));
    return spaced.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9+]+")), Qt::SkipEmptyParts);
}

int32 ScoreTemplate(const RenderGraphTemplate& entry, const QStringList& tokens)
{
    const QString label = entry.Label.toLower();
    const QStringList words = SplitWords(entry.Label) + SplitWords(entry.Identity) + SplitWords(entry.Algorithm);
    const QString keywords = entry.Keywords.join(QLatin1Char(' ')) + QLatin1Char(' ') + entry.Identity.toLower();
    const QString category = GetRenderGraphTemplateCategoryLabel(entry.Category).toLower();
    const QString description = entry.Description.toLower();
    int32 total = 0;
    for (const QString& token : tokens)
    {
        int32 best = 0;
        for (const QString& word : words)
        {
            if (word.startsWith(token) == true)
            {
                best = LABEL_PREFIX_SCORE;
                break;
            }
        }
        if (best == 0 && label.contains(token) == true)
        {
            best = LABEL_SUBSTRING_SCORE;
        }
        if (best == 0 && keywords.contains(token) == true)
        {
            best = KEYWORD_SCORE;
        }
        if (best == 0 && category.contains(token) == true)
        {
            best = CATEGORY_SCORE;
        }
        if (best == 0 && description.contains(token) == true)
        {
            best = DESCRIPTION_SCORE;
        }
        if (best == 0)
        {
            return 0;
        }
        total += best;
    }
    if (label.startsWith(tokens.join(QLatin1Char(' '))) == true)
    {
        total += WHOLE_LABEL_PREFIX_BONUS;
    }
    return total;
}

/// Kind glyph drawn to match the canvas: one square for a node, two overlapping for a snippet.
QIcon MakeKindIcon(RenderGraphTemplateKind kind)
{
    QPixmap pixmap(ICON_SIZE, ICON_SIZE);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool isSnippet = kind == RenderGraphTemplateKind::Snippet;
    painter.setPen(
        QPen(QColor::fromRgba(isSnippet == true ? SNIPPET_GLYPH_COLOR : NODE_GLYPH_COLOR), GLYPH_STROKE_WIDTH));
    if (isSnippet == true)
    {
        painter.drawRoundedRect(
            QRectF(SNIPPET_GLYPH_NEAR, SNIPPET_GLYPH_FAR, SNIPPET_GLYPH_EXTENT, SNIPPET_GLYPH_EXTENT),
            SNIPPET_GLYPH_RADIUS,
            SNIPPET_GLYPH_RADIUS);
        painter.drawRoundedRect(
            QRectF(SNIPPET_GLYPH_FAR, SNIPPET_GLYPH_NEAR, SNIPPET_GLYPH_EXTENT, SNIPPET_GLYPH_EXTENT),
            SNIPPET_GLYPH_RADIUS,
            SNIPPET_GLYPH_RADIUS);
    }
    else
    {
        painter.drawRoundedRect(QRectF(NODE_GLYPH_INSET, NODE_GLYPH_INSET, NODE_GLYPH_EXTENT, NODE_GLYPH_EXTENT),
                                NODE_GLYPH_RADIUS,
                                NODE_GLYPH_RADIUS);
    }
    return QIcon(pixmap);
}

QString ShortenType(const QString& type)
{
    if (type == QStringLiteral("ColorTexture"))
    {
        return QStringLiteral("Color");
    }
    if (type == QStringLiteral("DepthTexture"))
    {
        return QStringLiteral("Depth");
    }
    if (type == QStringLiteral("RenderSize"))
    {
        return QStringLiteral("Size");
    }
    if (type.startsWith(QStringLiteral("Scene")) == true && type != QStringLiteral("Scene"))
    {
        return type.mid(QStringLiteral("Scene").size());
    }
    return type;
}

QString SummarizePorts(const QVector<RenderGraphPort>& ports)
{
    constexpr qsizetype MAX_LISTED_TYPES = 2;
    if (ports.isEmpty() == true)
    {
        return QStringLiteral("—");
    }
    QStringList types;
    for (qsizetype i = 0; i < ports.size() && i < MAX_LISTED_TYPES; ++i)
    {
        types.append(ShortenType(ports.at(i).Type));
    }
    QString text = types.join(QStringLiteral(", "));
    if (ports.size() > MAX_LISTED_TYPES)
    {
        text += QStringLiteral(" +%1").arg(ports.size() - MAX_LISTED_TYPES);
    }
    return text;
}

QString ListPorts(const QVector<RenderGraphPort>& ports)
{
    QStringList entries;
    for (const RenderGraphPort& port : ports)
    {
        entries.append(QStringLiteral("%1 (%2)").arg(port.Id, port.Type));
    }
    return entries.isEmpty() == true ? QStringLiteral("none") : entries.join(QStringLiteral(", "));
}

QTreeWidgetItem* MakeHeading(QTreeWidget& inoutTree, const QString& text)
{
    QTreeWidgetItem* item = new QTreeWidgetItem(&inoutTree, {text.toUpper()});
    item->setFlags(Qt::ItemIsEnabled);
    QFont font = item->font(0);
    font.setPointSizeF(font.pointSizeF() * HEADING_FONT_SCALE);
    font.setLetterSpacing(QFont::PercentageSpacing, HEADING_LETTER_SPACING);
    item->setFont(0, font);
    item->setForeground(0, inoutTree.palette().color(QPalette::PlaceholderText));
    item->setFirstColumnSpanned(true);
    return item;
}
} // namespace

QString GetRenderGraphTemplateKey(const RenderGraphTemplate& entry)
{
    const QString kind = entry.Kind == RenderGraphTemplateKind::Graph     ? QStringLiteral("graph")
                         : entry.Kind == RenderGraphTemplateKind::Snippet ? QStringLiteral("snippet")
                                                                          : QStringLiteral("node");
    QString key = kind + QLatin1Char(':') + entry.Identity;
    if (entry.Algorithm.isEmpty() == false)
    {
        key += QLatin1Char('/') + entry.Algorithm;
    }
    return key;
}

QVector<RenderGraphTemplate> SearchRenderGraphTemplates(const QString& query,
                                                        const QVector<RenderGraphTemplate>& templates)
{
    struct Ranked final
    {
        int32 Score = 0;
        int32 Index = 0;
    };
    const QStringList tokens = query.simplified().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QVector<Ranked> ranked;
    for (qsizetype i = 0; i < templates.size(); ++i)
    {
        const RenderGraphTemplate& entry = templates.at(i);
        if (tokens.isEmpty() == true)
        {
            // Group by category in vocabulary order; a lower score sorts first after negation below.
            const int32 order =
                static_cast<int32>(GetRenderGraphTemplateCategories(entry.Kind).indexOf(entry.Category));
            ranked.append({-order, static_cast<int32>(i)});
            continue;
        }
        const int32 score = ScoreTemplate(entry, tokens);
        if (score > 0)
        {
            ranked.append({score, static_cast<int32>(i)});
        }
    }
    QVector<Ranked> scratch(ranked.size());
    // StableSort fails only for null or aliased arrays; these are separate allocations, and a failure
    // would leave the input order, which is still a usable result.
    static_cast<void>(StableSort(ranked.data(),
                                 scratch.data(),
                                 static_cast<usize>(ranked.size()),
                                 [](const Ranked& a, const Ranked& b) noexcept { return a.Score > b.Score; }));
    QVector<RenderGraphTemplate> result;
    result.reserve(ranked.size());
    for (const Ranked& entry : ranked)
    {
        result.append(templates.at(entry.Index));
    }
    return result;
}

RenderGraphQuickAdd::RenderGraphQuickAdd(const RenderGraphTemplateRegistry& registry, QWidget* parentOrNull)
    : QFrame(parentOrNull, Qt::Popup), mSearch(new QLineEdit(this)), mContext(new QWidget(this)),
      mContextLabel(new QLabel(mContext)), mContextClear(new QToolButton(mContext)), mKindAll(new QToolButton(this)),
      mKindSnippets(new QToolButton(this)), mKindNodes(new QToolButton(this)), mCategories(new QTreeWidget(this)),
      mResults(new QTreeWidget(this)), mMore(new QLabel(this)), mGraphHint(new QPushButton(this)),
      mTitle(new QLabel(this)), mDescription(new QLabel(this)), mPorts(new QLabel(this)), mWarning(new QLabel(this)),
      mInsert(new QPushButton(this)), mPin(new QPushButton(this))
{
    setObjectName(QStringLiteral("renderGraphQuickAdd"));
    setAccessibleName(QStringLiteral("Quick Add"));
    setFrameShape(QFrame::StyledPanel);
    setFixedWidth(PALETTE_WIDTH);
    mTemplates =
        registry.GetTemplates(RenderGraphTemplateKind::Snippet) + registry.GetTemplates(RenderGraphTemplateKind::Node);
    mGraphs = registry.GetTemplates(RenderGraphTemplateKind::Graph);
    mCategory = GetSuggestedCategory();

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(PALETTE_MARGIN, PALETTE_MARGIN, PALETTE_MARGIN, SECTION_SPACING);
    layout->setSpacing(SECTION_SPACING);

    QHBoxLayout* head = new QHBoxLayout;
    mSearch->setObjectName(QStringLiteral("renderGraphQuickAddSearch"));
    mSearch->setPlaceholderText(QStringLiteral("Search snippets and nodes"));
    mSearch->setAccessibleName(QStringLiteral("Search templates"));
    mSearch->setClearButtonEnabled(true);
    mSearch->installEventFilter(this);
    head->addWidget(mSearch, 1);
    QButtonGroup* kinds = new QButtonGroup(this);
    kinds->setExclusive(true);
    const auto setUpKind = [this, kinds, head](QToolButton* button,
                                               const QString& name,
                                               RenderGraphQuickAddKind kind,
                                               const QString& text) {
        button->setObjectName(name);
        button->setText(text);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(QStringLiteral("Tab switches kinds while searching."));
        kinds->addButton(button);
        head->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, kind]() { SetKind(kind); });
    };
    setUpKind(mKindAll,
              QStringLiteral("renderGraphQuickAddKindAll"),
              RenderGraphQuickAddKind::All,
              QStringLiteral("All"));
    setUpKind(mKindSnippets,
              QStringLiteral("renderGraphQuickAddKindSnippets"),
              RenderGraphQuickAddKind::Snippets,
              QStringLiteral("Snippets"));
    setUpKind(mKindNodes,
              QStringLiteral("renderGraphQuickAddKindNodes"),
              RenderGraphQuickAddKind::Nodes,
              QStringLiteral("Nodes"));
    layout->addLayout(head);

    // Shown only with a port filter: which output the new node connects from, and a way out.
    mContext->setObjectName(QStringLiteral("renderGraphQuickAddContext"));
    QHBoxLayout* context = new QHBoxLayout(mContext);
    context->setContentsMargins(0, 0, 0, 0);
    mContextLabel->setObjectName(QStringLiteral("renderGraphQuickAddContextLabel"));
    mContextLabel->setTextFormat(Qt::PlainText);
    context->addWidget(mContextLabel, 1);
    mContextClear->setObjectName(QStringLiteral("renderGraphQuickAddContextClear"));
    mContextClear->setText(QStringLiteral("Show all"));
    mContextClear->setAccessibleName(QStringLiteral("Clear the connection filter"));
    mContextClear->setToolTip(
        QStringLiteral("List every snippet and node; the added node is not connected. Backspace in an empty "
                       "search does the same."));
    mContextClear->setFocusPolicy(Qt::NoFocus);
    connect(mContextClear, &QToolButton::clicked, this, [this]() {
        ClearPortFilter();
        mSearch->setFocus();
    });
    context->addWidget(mContextClear);
    mContext->hide();
    layout->addWidget(mContext);

    const int32 rowHeight = qMax(22, fontMetrics().height() + 8);
    // Room for every result row plus the Recent and Pinned headings, so the list never scrolls.
    const int32 listHeight = (MAX_ROWS + 2) * rowHeight + 4;
    QHBoxLayout* body = new QHBoxLayout;
    body->setSpacing(SECTION_SPACING);
    mCategories->setObjectName(QStringLiteral("renderGraphQuickAddCategories"));
    mCategories->setAccessibleName(QStringLiteral("Template categories"));
    mCategories->setColumnCount(2);
    mCategories->setHeaderHidden(true);
    mCategories->setRootIsDecorated(false);
    mCategories->setFocusPolicy(Qt::NoFocus);
    mCategories->setFixedWidth(CATEGORY_WIDTH);
    mCategories->setFixedHeight(listHeight);
    mCategories->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mCategories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mCategories->header()->setStretchLastSection(false);
    mCategories->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mCategories->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(mCategories, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        SetCategory(item->data(0, CATEGORY_ROLE).toString());
        mSearch->setFocus();
    });
    body->addWidget(mCategories);

    QVBoxLayout* resultColumn = new QVBoxLayout;
    resultColumn->setSpacing(2);
    mResults->setObjectName(QStringLiteral("renderGraphQuickAddResults"));
    mResults->setAccessibleName(QStringLiteral("Templates"));
    mResults->setColumnCount(RESULT_COLUMN_COUNT);
    mResults->setHeaderHidden(true);
    mResults->setRootIsDecorated(false);
    mResults->setUniformRowHeights(true);
    mResults->setFocusPolicy(Qt::NoFocus);
    mResults->setFixedHeight(listHeight);
    mResults->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mResults->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mResults->setMouseTracking(true);
    mResults->viewport()->setMouseTracking(true);
    mResults->header()->setStretchLastSection(false);
    mResults->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mResults->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    mResults->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    mResults->header()->resizeSection(1, SIGNATURE_WIDTH);
    mResults->header()->resizeSection(2, PIN_WIDTH);
    connect(mResults, &QTreeWidget::itemEntered, this, [this](QTreeWidgetItem* item) {
        const QVariant index = item->data(0, LISTED_INDEX_ROLE);
        if (index.isValid() == true && index.toInt() != mHighlight)
        {
            mHighlight = index.toInt();
            mResults->setCurrentItem(item);
            renderDetail();
        }
    });
    connect(mResults, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int32 column) {
        const QVariant index = item->data(0, LISTED_INDEX_ROLE);
        if (index.isValid() == false)
        {
            return;
        }
        mHighlight = index.toInt();
        if (column == 2)
        {
            TogglePinned(GetRenderGraphTemplateKey(mListed.at(mHighlight)));
            mSearch->setFocus();
            return;
        }
        static_cast<void>(InsertHighlighted());
    });
    resultColumn->addWidget(mResults);
    mMore->setObjectName(QStringLiteral("renderGraphQuickAddMore"));
    mMore->setForegroundRole(QPalette::PlaceholderText);
    resultColumn->addWidget(mMore);
    mGraphHint->setObjectName(QStringLiteral("renderGraphQuickAddGraphHint"));
    mGraphHint->setFlat(true);
    mGraphHint->setFocusPolicy(Qt::NoFocus);
    mGraphHint->setCursor(Qt::PointingHandCursor);
    mGraphHint->setStyleSheet(QStringLiteral("text-align: left;"));
    connect(mGraphHint, &QPushButton::clicked, this, [this]() {
        const QString identity = mGraphMatch;
        close();
        Q_EMIT NewGraphRequested(identity);
    });
    resultColumn->addWidget(mGraphHint);
    body->addLayout(resultColumn, 1);
    layout->addLayout(body);

    QFrame* detail = new QFrame(this);
    detail->setObjectName(QStringLiteral("renderGraphQuickAddDetail"));
    detail->setFrameShape(QFrame::HLine);
    layout->addWidget(detail);
    mTitle->setObjectName(QStringLiteral("renderGraphQuickAddTitle"));
    mTitle->setTextFormat(Qt::PlainText);
    QFont titleFont = mTitle->font();
    titleFont.setBold(true);
    mTitle->setFont(titleFont);
    layout->addWidget(mTitle);
    mDescription->setObjectName(QStringLiteral("renderGraphQuickAddDescription"));
    mDescription->setTextFormat(Qt::PlainText);
    mDescription->setWordWrap(true);
    mDescription->setMinimumHeight(fontMetrics().lineSpacing() * 2);
    mDescription->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(mDescription);
    mPorts->setObjectName(QStringLiteral("renderGraphQuickAddPorts"));
    mPorts->setTextFormat(Qt::PlainText);
    mPorts->setWordWrap(true);
    mPorts->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(mPorts);
    mWarning->setObjectName(QStringLiteral("renderGraphQuickAddWarning"));
    mWarning->setTextFormat(Qt::PlainText);
    mWarning->setWordWrap(true);
    layout->addWidget(mWarning);

    QHBoxLayout* foot = new QHBoxLayout;
    QLabel* keys = new QLabel(QStringLiteral("↑↓ move · Enter insert · Tab kind · Esc close"), this);
    keys->setObjectName(QStringLiteral("renderGraphQuickAddKeys"));
    keys->setForegroundRole(QPalette::PlaceholderText);
    foot->addWidget(keys, 1);
    mPin->setObjectName(QStringLiteral("renderGraphQuickAddPin"));
    mPin->setFocusPolicy(Qt::NoFocus);
    connect(mPin, &QPushButton::clicked, this, [this]() {
        if (mHighlight < mListed.size())
        {
            TogglePinned(GetRenderGraphTemplateKey(mListed.at(mHighlight)));
        }
        mSearch->setFocus();
    });
    foot->addWidget(mPin);
    mInsert->setObjectName(QStringLiteral("renderGraphQuickAddInsert"));
    mInsert->setDefault(true);
    mInsert->setFocusPolicy(Qt::NoFocus);
    connect(mInsert, &QPushButton::clicked, this, [this]() { static_cast<void>(InsertHighlighted()); });
    foot->addWidget(mInsert);
    layout->addLayout(foot);

    connect(mSearch, &QLineEdit::textChanged, this, [this]() {
        mCategory = mSearch->text().trimmed().isEmpty() == true ? getDefaultCategory() : GetAllCategory();
        mHighlight = 0;
        refresh();
    });
    mKindAll->setChecked(true);
    refresh();
}

void RenderGraphQuickAdd::Open(const QPoint& globalPosition, const RenderGraphQuickAddPortFilter& filter)
{
    const QSignalBlocker blocker(mSearch);
    mSearch->clear();
    mPortFilter = filter;
    mCategory = getDefaultCategory();
    SetKind(RenderGraphQuickAddKind::All);
    adjustSize();
    QScreen* screenOrNull = QGuiApplication::screenAt(globalPosition);
    if (screenOrNull == nullptr)
    {
        screenOrNull = screen();
    }
    QPoint position = globalPosition;
    if (screenOrNull != nullptr)
    {
        const QRect available = screenOrNull->availableGeometry();
        position.setX(qBound(available.left(), position.x(), qMax(available.left(), available.right() - width())));
        position.setY(qBound(available.top(), position.y(), qMax(available.top(), available.bottom() - height())));
    }
    move(position);
    show();
    raise();
    activateWindow();
    mSearch->setFocus(Qt::PopupFocusReason);
}

const RenderGraphQuickAddPortFilter& RenderGraphQuickAdd::GetPortFilter() const noexcept
{
    return mPortFilter;
}

void RenderGraphQuickAdd::ClearPortFilter()
{
    mPortFilter = {};
    mCategory = mSearch->text().trimmed().isEmpty() == true ? getDefaultCategory() : GetAllCategory();
    mHighlight = 0;
    SetKind(mKind);
}

void RenderGraphQuickAdd::SetNodePorts(const QHash<QString, RenderGraphNodePorts>& ports)
{
    mNodePorts = ports;
    refresh();
}

void RenderGraphQuickAdd::SetUnavailableRequirements(const QHash<QString, QString>& messages)
{
    mUnavailableRequirements = messages;
    refresh();
}

void RenderGraphQuickAdd::SetRecent(const QStringList& keys)
{
    mRecent = keys.mid(0, MAX_RECENT);
    refresh();
}

void RenderGraphQuickAdd::SetPinned(const QStringList& keys)
{
    mPinned = keys;
    refresh();
}

const QStringList& RenderGraphQuickAdd::GetRecent() const noexcept
{
    return mRecent;
}

const QStringList& RenderGraphQuickAdd::GetPinned() const noexcept
{
    return mPinned;
}

RenderGraphQuickAddKind RenderGraphQuickAdd::GetKind() const noexcept
{
    return mKind;
}

void RenderGraphQuickAdd::SetKind(RenderGraphQuickAddKind kind)
{
    mKind = kind;
    mKindAll->setChecked(kind == RenderGraphQuickAddKind::All);
    mKindSnippets->setChecked(kind == RenderGraphQuickAddKind::Snippets);
    mKindNodes->setChecked(kind == RenderGraphQuickAddKind::Nodes);
    mSearch->setPlaceholderText(hasPortFilter() == true
                                    ? QStringLiteral("Search nodes that accept %1").arg(mPortFilter.Type)
                                : kind == RenderGraphQuickAddKind::Snippets ? QStringLiteral("Search snippets")
                                : kind == RenderGraphQuickAddKind::Nodes ? QStringLiteral("Search nodes")
                                                                         : QStringLiteral("Search snippets and nodes"));
    // Snippets apply through a dialog as a separate edit, so they cannot be connected on insert.
    mKindSnippets->setEnabled(hasPortFilter() == false);
    mKindSnippets->setToolTip(hasPortFilter() == true
                                  ? QStringLiteral("Snippets cannot be connected on insert. Show all to apply one.")
                                  : QStringLiteral("Tab switches kinds while searching."));
    mHighlight = 0;
    refresh();
}

const QString& RenderGraphQuickAdd::GetCategory() const noexcept
{
    return mCategory;
}

void RenderGraphQuickAdd::SetCategory(const QString& category)
{
    mCategory = category;
    mHighlight = 0;
    refresh();
}

QStringList RenderGraphQuickAdd::GetListedKeys() const
{
    QStringList keys;
    for (const RenderGraphTemplate& entry : mListed)
    {
        keys.append(GetRenderGraphTemplateKey(entry));
    }
    return keys;
}

int32 RenderGraphQuickAdd::GetHiddenCount() const noexcept
{
    return mHiddenCount;
}

QString RenderGraphQuickAdd::GetHighlightedKey() const
{
    return mHighlight < mListed.size() ? GetRenderGraphTemplateKey(mListed.at(mHighlight)) : QString{};
}

bool RenderGraphQuickAdd::InsertHighlighted()
{
    if (mHighlight >= mListed.size())
    {
        return false;
    }
    const RenderGraphTemplate entry = mListed.at(mHighlight);
    const QString key = GetRenderGraphTemplateKey(entry);
    const QString input = mPortFilter.Inputs.value(key);
    mRecent.removeAll(key);
    mRecent.prepend(key);
    mRecent = mRecent.mid(0, MAX_RECENT);
    Q_EMIT HistoryChanged();
    close();
    Q_EMIT InsertRequested(entry, input);
    return true;
}

void RenderGraphQuickAdd::TogglePinned(const QString& key)
{
    if (mPinned.removeAll(key) == 0)
    {
        mPinned.append(key);
    }
    Q_EMIT HistoryChanged();
    const QString highlighted = GetHighlightedKey();
    refresh();
    // Keep the same template highlighted when pinning reorders the Suggested list.
    const qsizetype index = GetListedKeys().indexOf(highlighted);
    if (index >= 0)
    {
        mHighlight = static_cast<int32>(index);
        refresh();
    }
}

bool RenderGraphQuickAdd::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != mSearch || event->type() != QEvent::KeyPress)
    {
        return QFrame::eventFilter(watched, event);
    }
    const QKeyEvent* key = static_cast<QKeyEvent*>(event);
    switch (key->key())
    {
        case Qt::Key_Down:
            moveHighlight(1);
            return true;
        case Qt::Key_Up:
            moveHighlight(-1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            static_cast<void>(InsertHighlighted());
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        case Qt::Key_Backspace:
            if (hasPortFilter() == true && mSearch->text().isEmpty() == true)
            {
                ClearPortFilter();
                return true;
            }
            return QFrame::eventFilter(watched, event);
        case Qt::Key_Tab:
            cycleKind(true);
            return true;
        case Qt::Key_Backtab:
            cycleKind(false);
            return true;
        default:
            return QFrame::eventFilter(watched, event);
    }
}

void RenderGraphQuickAdd::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        close();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void RenderGraphQuickAdd::moveHighlight(int32 delta)
{
    if (mListed.isEmpty() == true)
    {
        return;
    }
    mHighlight = qBound(0, mHighlight + delta, static_cast<int32>(mListed.size()) - 1);
    refresh();
}

void RenderGraphQuickAdd::cycleKind(bool isForward)
{
    constexpr int32 KIND_COUNT = 3;
    int32 next = (static_cast<int32>(mKind) + (isForward == true ? 1 : KIND_COUNT - 1)) % KIND_COUNT;
    if (hasPortFilter() == true && static_cast<RenderGraphQuickAddKind>(next) == RenderGraphQuickAddKind::Snippets)
    {
        next = (next + (isForward == true ? 1 : KIND_COUNT - 1)) % KIND_COUNT;
    }
    SetKind(static_cast<RenderGraphQuickAddKind>(next));
}

bool RenderGraphQuickAdd::hasPortFilter() const noexcept
{
    return mPortFilter.Source.isEmpty() == false;
}

QString RenderGraphQuickAdd::getDefaultCategory() const
{
    // A port filter already removes most choices, so it opens on everything compatible.
    return hasPortFilter() == true ? GetAllCategory() : GetSuggestedCategory();
}

QVector<RenderGraphTemplate> RenderGraphQuickAdd::getMatches() const
{
    QVector<RenderGraphTemplate> candidates;
    for (const RenderGraphTemplate& entry : mTemplates)
    {
        const bool isSnippet = entry.Kind == RenderGraphTemplateKind::Snippet;
        if ((mKind == RenderGraphQuickAddKind::Snippets && isSnippet == false) ||
            (mKind == RenderGraphQuickAddKind::Nodes && isSnippet == true))
        {
            continue;
        }
        if (hasPortFilter() == true && mPortFilter.Inputs.contains(GetRenderGraphTemplateKey(entry)) == false)
        {
            continue;
        }
        candidates.append(entry);
    }
    return SearchRenderGraphTemplates(mSearch->text(), candidates);
}

QString RenderGraphQuickAdd::getUnavailableMessage(const RenderGraphTemplate& entry) const
{
    for (const QString& requirement : entry.Requirements)
    {
        const QString message = mUnavailableRequirements.value(requirement);
        if (message.isEmpty() == false)
        {
            return message;
        }
    }
    return {};
}

QString RenderGraphQuickAdd::describePorts(const RenderGraphTemplate& entry) const
{
    if (entry.Kind == RenderGraphTemplateKind::Snippet)
    {
        return QStringLiteral("Applies as a library configuration named %1; on the root canvas it also adds a "
                              "Subgraph node that references it.")
            .arg(entry.DefaultConfiguration);
    }
    const QString key = GetRenderGraphTemplateKey(entry);
    if (mNodePorts.contains(key) == false)
    {
        return entry.Identity == QStringLiteral("Subgraph")
                   ? QStringLiteral("Ports come from the referenced configuration's inputs and outputs.")
                   : QString{};
    }
    const RenderGraphNodePorts ports = mNodePorts.value(key);
    QString text = QStringLiteral("Inputs: %1\nOutputs: %2").arg(ListPorts(ports.Inputs), ListPorts(ports.Outputs));
    if (mPortFilter.Inputs.contains(key) == true)
    {
        text += QStringLiteral("\nConnects %1 to its %2 input.").arg(mPortFilter.Source, mPortFilter.Inputs.value(key));
    }
    return text;
}

void RenderGraphQuickAdd::refresh()
{
    const QVector<RenderGraphTemplate> matches = getMatches();
    const bool isSearching = mSearch->text().trimmed().isEmpty() == false;
    mContextLabel->setText(QStringLiteral("Connect from %1 · %2").arg(mPortFilter.Source, mPortFilter.Type));
    mContext->setVisible(hasPortFilter() == true);
    if (isSearching == true && mCategory == GetSuggestedCategory())
    {
        mCategory = GetAllCategory();
    }

    QHash<QString, int32> counts;
    for (const RenderGraphTemplate& entry : matches)
    {
        counts[entry.Category] += 1;
    }
    mCategories->clear();
    const auto addCategory = [this](const QString& identity, int32 count, bool hasCount, const QString& label) {
        QTreeWidgetItem* item =
            new QTreeWidgetItem(mCategories, {label, hasCount == true ? QString::number(count) : QString{}});
        item->setData(0, CATEGORY_ROLE, identity);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (hasCount == true && count == 0)
        {
            item->setForeground(0, mCategories->palette().color(QPalette::PlaceholderText));
            item->setForeground(1, mCategories->palette().color(QPalette::PlaceholderText));
        }
        if (identity == mCategory)
        {
            mCategories->setCurrentItem(item);
        }
    };
    if (isSearching == false)
    {
        addCategory(GetSuggestedCategory(), 0, false, QStringLiteral("Suggested"));
    }
    addCategory(GetAllCategory(),
                static_cast<int32>(matches.size()),
                true,
                isSearching == true ? QStringLiteral("All matches") : QStringLiteral("Everything"));
    for (const QString& category : GetRenderGraphTemplateCategories(RenderGraphTemplateKind::Node))
    {
        addCategory(category, counts.value(category), true, GetRenderGraphTemplateCategoryLabel(category));
    }

    mResults->clear();
    mListed.clear();
    mHiddenCount = 0;
    const auto addRow = [this](const RenderGraphTemplate& entry) {
        QStringList tags;
        if (entry.IsDraft == true)
        {
            tags.append(QStringLiteral("draft"));
        }
        if (getUnavailableMessage(entry).isEmpty() == false)
        {
            tags.append(QStringLiteral("unavailable here"));
        }
        const QString label = tags.isEmpty() == true
                                  ? entry.Label
                                  : QStringLiteral("%1  (%2)").arg(entry.Label, tags.join(QStringLiteral(", ")));
        const QString key = GetRenderGraphTemplateKey(entry);
        QString signature = QStringLiteral("library configuration");
        if (entry.Kind == RenderGraphTemplateKind::Node)
        {
            const RenderGraphNodePorts ports = mNodePorts.value(key);
            signature = mNodePorts.contains(key) == true
                            ? SummarizePorts(ports.Inputs) + QStringLiteral(" → ") + SummarizePorts(ports.Outputs)
                            : QString{};
        }
        const bool isPinned = mPinned.contains(key);
        QTreeWidgetItem* item =
            new QTreeWidgetItem(mResults,
                                {label, signature, isPinned == true ? QStringLiteral("★") : QStringLiteral("☆")});
        item->setIcon(0, MakeKindIcon(entry.Kind));
        item->setData(0, LISTED_INDEX_ROLE, static_cast<int32>(mListed.size()));
        item->setToolTip(0, entry.Description);
        item->setToolTip(2,
                         isPinned == true ? QStringLiteral("Unpin from Suggested")
                                          : QStringLiteral("Pin to Suggested"));
        item->setForeground(1, mResults->palette().color(QPalette::PlaceholderText));
        item->setTextAlignment(2, Qt::AlignCenter);
        mListed.append(entry);
    };
    const auto addPlaceholder = [this](const QString& text) {
        QTreeWidgetItem* item = new QTreeWidgetItem(mResults, {text});
        item->setFlags(Qt::ItemIsEnabled);
        item->setForeground(0, mResults->palette().color(QPalette::PlaceholderText));
        item->setFirstColumnSpanned(true);
    };
    QString more;
    if (mCategory == GetSuggestedCategory())
    {
        const auto find = [&matches](const QString& key, RenderGraphTemplate& outEntry) {
            for (const RenderGraphTemplate& entry : matches)
            {
                if (GetRenderGraphTemplateKey(entry) == key)
                {
                    outEntry = entry;
                    return true;
                }
            }
            return false;
        };
        QVector<RenderGraphTemplate> recent;
        for (const QString& key : mRecent)
        {
            RenderGraphTemplate entry;
            if (find(key, entry) == true)
            {
                recent.append(entry);
            }
        }
        QVector<RenderGraphTemplate> pinned;
        for (const QString& key : mPinned)
        {
            RenderGraphTemplate entry;
            if (mRecent.contains(key) == false && recent.size() + pinned.size() < MAX_ROWS && find(key, entry) == true)
            {
                pinned.append(entry);
            }
        }
        if (recent.isEmpty() == false)
        {
            MakeHeading(*mResults, QStringLiteral("Recent"));
            for (const RenderGraphTemplate& entry : recent)
            {
                addRow(entry);
            }
        }
        if (pinned.isEmpty() == false)
        {
            MakeHeading(*mResults, QStringLiteral("Pinned"));
            for (const RenderGraphTemplate& entry : pinned)
            {
                addRow(entry);
            }
        }
        if (mListed.isEmpty() == true)
        {
            addPlaceholder(QStringLiteral("Nothing recent or pinned yet. Type to search, or pick a category."));
        }
        more = QStringLiteral("☆ pins a template here · %1 snippets and nodes").arg(mTemplates.size());
    }
    else
    {
        QVector<RenderGraphTemplate> filtered;
        for (const RenderGraphTemplate& entry : matches)
        {
            if (mCategory == GetAllCategory() || entry.Category == mCategory)
            {
                filtered.append(entry);
            }
        }
        for (qsizetype i = 0; i < filtered.size() && i < MAX_ROWS; ++i)
        {
            addRow(filtered.at(i));
        }
        mHiddenCount = static_cast<int32>(filtered.size() - mListed.size());
        if (filtered.isEmpty() == true)
        {
            const QString nothing =
                hasPortFilter() == true && isSearching == false
                    ? QStringLiteral("No node accepts %1. Show all to add an unconnected node.").arg(mPortFilter.Type)
                : hasPortFilter() == true ? QStringLiteral("No compatible node matches. Try another word or Show all.")
                                          : QStringLiteral("Nothing matches. Try another word or switch to All.");
            addPlaceholder(matches.isEmpty() == true
                               ? nothing
                               : QStringLiteral("No matches here; %1 in other categories.").arg(matches.size()));
        }
        if (mHiddenCount > 0)
        {
            more = mCategory == GetAllCategory()
                       ? QStringLiteral("+%1 more · keep typing or pick a category").arg(mHiddenCount)
                       : QStringLiteral("+%1 more in %2 · keep typing")
                             .arg(mHiddenCount)
                             .arg(GetRenderGraphTemplateCategoryLabel(mCategory));
        }
    }
    mMore->setText(more);
    mMore->setVisible(more.isEmpty() == false);

    mGraphMatch.clear();
    int32 graphCount = 0;
    if (isSearching == true)
    {
        const QVector<RenderGraphTemplate> graphs = SearchRenderGraphTemplates(mSearch->text(), mGraphs);
        graphCount = static_cast<int32>(graphs.size());
        if (graphs.isEmpty() == false)
        {
            mGraphMatch = graphs.first().Identity;
        }
    }
    mGraphHint->setText(graphCount == 1
                            ? QStringLiteral("1 complete graph matches · New Render Graph…")
                            : QStringLiteral("%1 complete graphs match · New Render Graph…").arg(graphCount));
    mGraphHint->setVisible(graphCount > 0);

    mHighlight = mListed.isEmpty() == true ? 0 : qBound(0, mHighlight, static_cast<int32>(mListed.size()) - 1);
    for (int32 i = 0; i < mResults->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = mResults->topLevelItem(i);
        const QVariant index = item->data(0, LISTED_INDEX_ROLE);
        if (index.isValid() == true && index.toInt() == mHighlight)
        {
            mResults->setCurrentItem(item);
        }
    }
    renderDetail();
}

void RenderGraphQuickAdd::renderDetail()
{
    if (mHighlight >= mListed.size())
    {
        mTitle->setText(QStringLiteral("No template selected"));
        mDescription->setText(QStringLiteral("Search by name, purpose or port type, or pick a category."));
        mPorts->clear();
        mWarning->hide();
        mInsert->setEnabled(false);
        mInsert->setText(QStringLiteral("Insert"));
        mPin->setEnabled(false);
        mPin->setText(QStringLiteral("Pin"));
        return;
    }
    const RenderGraphTemplate& entry = mListed.at(mHighlight);
    // Plain nodes are labelled by their kind; only presets such as Upscale name it separately.
    const QString kind = entry.Kind == RenderGraphTemplateKind::Snippet ? QStringLiteral("Snippet")
                         : entry.Label == entry.Identity                ? QStringLiteral("Node")
                                                                        : QStringLiteral("%1 node").arg(entry.Identity);
    QStringList facts{kind, GetRenderGraphTemplateCategoryLabel(entry.Category)};
    for (const QString& requirement : entry.Requirements)
    {
        facts.append(QStringLiteral("needs %1").arg(GetRenderGraphTemplateRequirementLabel(requirement)));
    }
    if (entry.IsDraft == true)
    {
        facts.append(QStringLiteral("draft preset"));
    }
    mTitle->setText(QStringLiteral("%1 — %2").arg(entry.Label, facts.join(QStringLiteral(" · "))));
    mDescription->setText(entry.Description);
    mPorts->setText(describePorts(entry));
    const QString warning = getUnavailableMessage(entry);
    mWarning->setText(warning);
    mWarning->setVisible(warning.isEmpty() == false);
    const bool isSnippet = entry.Kind == RenderGraphTemplateKind::Snippet;
    mInsert->setEnabled(true);
    mInsert->setText(isSnippet == true ? QStringLiteral("Apply…") : QStringLiteral("Insert"));
    mInsert->setToolTip(isSnippet == true ? QStringLiteral("Choose the configuration name, then apply the snippet.")
                        : hasPortFilter() == true
                            ? QStringLiteral("Add this node at the wire's end and connect it, as one undoable edit.")
                            : QStringLiteral("Add this node where Quick Add opened, as one undoable edit."));
    const bool isPinned = mPinned.contains(GetRenderGraphTemplateKey(entry));
    mPin->setEnabled(true);
    mPin->setText(isPinned == true ? QStringLiteral("Unpin") : QStringLiteral("Pin"));
}
} // namespace ludus::editor
