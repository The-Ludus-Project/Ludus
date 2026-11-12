#include "internal/inspector_section.h"

#include "design_tokens.h"
#include "internal/inspector_fields.h"
#include "internal/theme_fonts.h"

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace ludus::editor
{
using namespace foundation;
namespace
{
/// Side of a section's component icon and its lock, in logical pixels.
constexpr int32 SECTION_ICON_SIZE = design::SPACING_LARGE;
/// Share of the dock's width the label column takes, in percent, between the two limits below.
constexpr int32 LABEL_COLUMN_PERCENT = 35;
constexpr int32 PERCENT = 100;
/// The label column never shrinks below this many average characters ("Intensity", "Exposure"), nor grows beyond
/// the maximum, so wide docks give the room to values rather than to labels.
constexpr int32 LABEL_MINIMUM_CHARACTERS = 12;
constexpr int32 LABEL_MAXIMUM_CHARACTERS = 22;
/// Characters a shortened label keeps before its ellipsis at the very least.
constexpr int32 LABEL_KEPT_CHARACTERS = 3;
const char* const LABEL_COLUMN_PROPERTY = "inspectorLabelColumn";
const char* const INSPECTOR_FORM_PROPERTY = "isInspectorForm";

QString GetCollapsedSettingsKey(const QString& stateKey)
{
    return QStringLiteral("inspector/collapsedSections/") + stateKey;
}
} // namespace

InspectorLabel::InspectorLabel(const QString& text, QWidget* parentOrNull) : QLabel(text, parentOrNull)
{
    setTextFormat(Qt::PlainText);
    setWordWrap(false);
    setToolTip(text);
}

bool InspectorLabel::IsElided() const
{
    return wordWrap() == false && GetShownText() != text();
}

QString InspectorLabel::GetShownText() const
{
    return fontMetrics().elidedText(text(),
                                    Qt::ElideRight,
                                    contentsRect().width() - 2 * margin() - std::max(indent(), 0));
}

QSize InspectorLabel::sizeHint() const
{
    if (wordWrap() == true)
    {
        return QLabel::sizeHint();
    }
    // The whole text on one line, measured afresh: QLabel's own hint keeps the widest fixed width it was ever given.
    const QMargins margins = contentsMargins();
    return QSize(fontMetrics().horizontalAdvance(text()) + 2 * margin() + std::max(indent(), 0) + margins.left() +
                     margins.right(),
                 QLabel::sizeHint().height());
}

QSize InspectorLabel::minimumSizeHint() const
{
    if (wordWrap() == true || text().size() <= LABEL_KEPT_CHARACTERS)
    {
        return QLabel::minimumSizeHint();
    }
    const QString shortest = text().left(LABEL_KEPT_CHARACTERS) + QStringLiteral("…");
    const QMargins margins = contentsMargins();
    return QSize(fontMetrics().horizontalAdvance(shortest) + 2 * margin() + margins.left() + margins.right(),
                 QLabel::minimumSizeHint().height());
}

void InspectorLabel::mouseReleaseEvent(QMouseEvent* event)
{
    QAbstractButton* buttonOrNull = qobject_cast<QAbstractButton*>(buddy());
    if (event->button() == Qt::LeftButton && buttonOrNull != nullptr && buttonOrNull->isCheckable() == true &&
        buttonOrNull->isEnabled() == true && rect().contains(event->position().toPoint()) == true)
    {
        buttonOrNull->click();
        event->accept();
        return;
    }
    QLabel::mouseReleaseEvent(event);
}

void InspectorLabel::paintEvent(QPaintEvent* event)
{
    if (wordWrap() == true)
    {
        QLabel::paintEvent(event);
        return;
    }
    QPainter painter(this);
    const QRect area = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    style()->drawItemText(&painter, area, alignment(), palette(), isEnabled(), GetShownText(), foregroundRole());
}

InspectorLabel& AddInspectorRow(QFormLayout& inoutForm, const QString& title, QWidget& inoutField)
{
    InspectorLabel* label = new InspectorLabel(title, inoutForm.parentWidget());
    const QAbstractButton* buttonOrNull = qobject_cast<const QAbstractButton*>(&inoutField);
    if (buttonOrNull != nullptr && buttonOrNull->isCheckable() == true)
    {
        label->setBuddy(&inoutField);
        label->setCursor(Qt::PointingHandCursor);
    }
    if (inoutField.accessibleName().isEmpty() == true)
    {
        inoutField.setAccessibleName(title);
    }
    inoutForm.addRow(label, &inoutField);
    return *label;
}

InspectorSection::InspectorSection(const QString& title, const QString& stateKey, QWidget* parentOrNull)
    : QWidget(parentOrNull), mStateKey(stateKey), mHeader(new QWidget(this)), mDisclosure(new QToolButton(mHeader)),
      mIcon(new QLabel(mHeader)), mTitle(new QLabel(title, mHeader)), mRequired(new QLabel(mHeader)),
      mMenuButton(new QToolButton(mHeader)), mMenu(new QMenu(mMenuButton)), mHelp(new InspectorLabel(QString(), this)),
      mMore(new QToolButton(this)), mFields(new QWidget(this))
{
    setObjectName(QStringLiteral("inspectorSection"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, design::SPACING_MEDIUM, 0, design::SPACING_SMALL);
    layout->setSpacing(design::SPACING_SMALL);

    mHeader->setObjectName(QStringLiteral("inspectorSectionHeader"));
    mHeader->setCursor(Qt::PointingHandCursor);
    mHeader->installEventFilter(this);
    QHBoxLayout* headerLayout = new QHBoxLayout(mHeader);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(design::SPACING_EXTRA_SMALL);
    mDisclosure->setObjectName(QStringLiteral("inspectorSectionToggle"));
    mDisclosure->setAutoRaise(true);
    mDisclosure->setArrowType(Qt::DownArrow);
    mDisclosure->setIconSize(QSize(SECTION_ICON_SIZE, SECTION_ICON_SIZE));
    mDisclosure->setAccessibleName(QStringLiteral("Collapse %1").arg(title));
    mDisclosure->setToolTip(QStringLiteral("Collapse or expand %1").arg(title));
    connect(mDisclosure, &QToolButton::clicked, this, [this]() { SetCollapsed(mIsCollapsed == false); });
    mIcon->setObjectName(QStringLiteral("inspectorSectionIcon"));
    mIcon->setFixedSize(SECTION_ICON_SIZE, SECTION_ICON_SIZE);
    mIcon->hide();
    mTitle->setObjectName(QStringLiteral("inspectorSectionTitle"));
    mTitle->setTextFormat(Qt::PlainText);
    mTitle->setFont(MakeTypeFont(font(), TypeRole::Title));
    mTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mTitle->setMinimumWidth(mTitle->fontMetrics().horizontalAdvance(title.left(LABEL_KEPT_CHARACTERS)));
    mTitle->setToolTip(title);
    mRequired->setObjectName(QStringLiteral("inspectorSectionRequired"));
    mRequired->setFixedSize(SECTION_ICON_SIZE, SECTION_ICON_SIZE);
    mRequired->hide();
    mMenuButton->setObjectName(QStringLiteral("inspectorSectionMenu"));
    mMenuButton->setAutoRaise(true);
    mMenuButton->setIcon(GetEditorIcon(EditorIcon::More));
    mMenuButton->setIconSize(QSize(SECTION_ICON_SIZE, SECTION_ICON_SIZE));
    // Opened on click rather than through setMenu, which would add a second arrow beside the "⋯".
    connect(mMenuButton, &QToolButton::clicked, mMenu, [this]() {
        mMenu->popup(mMenuButton->mapToGlobal(QPoint(0, mMenuButton->height())));
    });
    mMenuButton->setAccessibleName(QStringLiteral("%1 actions").arg(title));
    mMenuButton->setToolTip(QStringLiteral("%1 actions").arg(title));
    mMenuButton->hide();
    headerLayout->addWidget(mDisclosure);
    headerLayout->addWidget(mIcon);
    headerLayout->addWidget(mTitle, 1);
    headerLayout->addWidget(mRequired);
    headerLayout->addWidget(mMenuButton);
    layout->addWidget(mHeader);

    QWidget* helpRow = new QWidget(this);
    helpRow->setObjectName(QStringLiteral("inspectorSectionHelpRow"));
    QHBoxLayout* helpLayout = new QHBoxLayout(helpRow);
    helpLayout->setContentsMargins(0, 0, 0, 0);
    helpLayout->setSpacing(design::SPACING_EXTRA_SMALL);
    mHelp->setParent(helpRow);
    mHelp->setObjectName(QStringLiteral("inspectorSectionHelp"));
    mHelp->setFont(MakeTypeFont(font(), TypeRole::Caption));
    mHelp->setForegroundRole(QPalette::PlaceholderText);
    mHelp->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mHelp->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mHelp->installEventFilter(this);
    mMore->setParent(helpRow);
    mMore->setObjectName(QStringLiteral("inspectorSectionMore"));
    mMore->setAutoRaise(true);
    mMore->setFont(MakeTypeFont(font(), TypeRole::Caption));
    mMore->setText(QStringLiteral("More"));
    mMore->setToolTip(QStringLiteral("Show the whole description"));
    mMore->hide();
    connect(mMore, &QToolButton::clicked, this, [this]() { SetHelpExpanded(mIsHelpExpanded == false); });
    helpLayout->addWidget(mHelp, 1);
    helpLayout->addWidget(mMore, 0, Qt::AlignTop);
    helpRow->hide();
    layout->addWidget(helpRow);

    mFields->setObjectName(QStringLiteral("inspectorSectionFields"));
    layout->addWidget(mFields);

    if (mStateKey.isEmpty() == false)
    {
        SetCollapsed(QSettings().value(GetCollapsedSettingsKey(mStateKey), false).toBool());
    }
}

QString InspectorSection::GetTitle() const
{
    return mTitle->text();
}

void InspectorSection::SetIcon(EditorIcon icon)
{
    mIcon->setPixmap(GetEditorIcon(icon).pixmap(QSize(SECTION_ICON_SIZE, SECTION_ICON_SIZE), devicePixelRatioF()));
    mIcon->show();
}

void InspectorSection::SetHelp(const QString& text)
{
    mHelp->setText(text);
    mHelp->setToolTip(text);
    mHelp->parentWidget()->setVisible(text.isEmpty() == false && mIsCollapsed == false);
    updateHelp();
}

void InspectorSection::SetRequired(const QString& reason)
{
    mRequired->setPixmap(
        GetEditorIcon(EditorIcon::Locked).pixmap(QSize(SECTION_ICON_SIZE, SECTION_ICON_SIZE), devicePixelRatioF()));
    mRequired->setToolTip(reason);
    mRequired->setAccessibleName(reason);
    mRequired->show();
}

QAction& InspectorSection::AddMenuAction(const QString& text, const QString& objectName)
{
    QAction* action = mMenu->addAction(text);
    action->setObjectName(objectName);
    mMenuButton->show();
    return *action;
}

void InspectorSection::SetCollapsed(bool isCollapsed)
{
    const bool hasChanged = mIsCollapsed != isCollapsed;
    mIsCollapsed = isCollapsed;
    mDisclosure->setArrowType(isCollapsed == true ? Qt::RightArrow : Qt::DownArrow);
    mDisclosure->setAccessibleName(
        (isCollapsed == true ? QStringLiteral("Expand %1") : QStringLiteral("Collapse %1")).arg(mTitle->text()));
    mFields->setVisible(isCollapsed == false);
    mHelp->parentWidget()->setVisible(isCollapsed == false && mHelp->text().isEmpty() == false);
    if (hasChanged == false)
    {
        return;
    }
    if (mStateKey.isEmpty() == false)
    {
        QSettings settings;
        if (isCollapsed == true)
        {
            settings.setValue(GetCollapsedSettingsKey(mStateKey), true);
        }
        else
        {
            settings.remove(GetCollapsedSettingsKey(mStateKey));
        }
    }
    Q_EMIT CollapsedChanged(isCollapsed);
}

void InspectorSection::SetHelpExpanded(bool isExpanded)
{
    mIsHelpExpanded = isExpanded;
    mHelp->setWordWrap(isExpanded);
    mHelp->setSizePolicy(isExpanded == true ? QSizePolicy::Preferred : QSizePolicy::Ignored, QSizePolicy::Preferred);
    mMore->setText(isExpanded == true ? QStringLiteral("Less") : QStringLiteral("More"));
    mMore->setToolTip(isExpanded == true ? QStringLiteral("Show the description on one line")
                                         : QStringLiteral("Show the whole description"));
    mHelp->updateGeometry();
    updateHelp();
}

bool InspectorSection::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mHeader && event->type() == QEvent::MouseButtonRelease)
    {
        const QMouseEvent* mouse = static_cast<const QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && mHeader->rect().contains(mouse->position().toPoint()) == true)
        {
            SetCollapsed(mIsCollapsed == false);
            return true;
        }
    }
    if (watched == mHelp && event->type() == QEvent::Resize)
    {
        updateHelp();
    }
    return QWidget::eventFilter(watched, event);
}

void InspectorSection::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    // A thin rule above the section separates it from the one before, with no card around its fields.
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::Midlight));
    painter.drawLine(0, 0, width() - 1, 0);
}

void InspectorSection::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateHelp();
}

void InspectorSection::updateHelp()
{
    // "More" appears only when the caption line had to shorten the help, or to fold it back once expanded.
    const bool hasMore = mIsHelpExpanded == true || (mHelp->text().isEmpty() == false && mHelp->IsElided() == true);
    if (mMore->isVisibleTo(this) != hasMore)
    {
        mMore->setVisible(hasMore);
    }
}

InspectorLabelColumn::InspectorLabelColumn(QWidget& inoutRoot) : QObject(&inoutRoot), mRoot(inoutRoot)
{
    mRoot.setProperty(LABEL_COLUMN_PROPERTY, QVariant::fromValue(static_cast<QObject*>(this)));
    mRoot.installEventFilter(this);
}

void InspectorLabelColumn::SetViewport(QWidget& inoutViewport)
{
    mViewport = &inoutViewport;
    inoutViewport.installEventFilter(this);
}

int32 InspectorLabelColumn::getAvailableWidth() const
{
    if (mViewport.isNull() == true || mViewport->isAncestorOf(&mRoot) == false)
    {
        return mRoot.contentsRect().width();
    }
    // The root sits inset in the viewport by its parents' margins; the same inset is kept on the right.
    const int32 inset = mRoot.mapTo(mViewport.data(), QPoint(0, 0)).x();
    return std::max(mViewport->width() - 2 * inset, 0);
}

const InspectorLabelColumn* InspectorLabelColumn::FindOrNull(const QWidget& widget)
{
    for (const QWidget* currentOrNull = &widget; currentOrNull != nullptr;
         currentOrNull = currentOrNull->parentWidget())
    {
        const QObject* columnOrNull = currentOrNull->property(LABEL_COLUMN_PROPERTY).value<QObject*>();
        if (columnOrNull != nullptr)
        {
            return dynamic_cast<const InspectorLabelColumn*>(columnOrNull);
        }
    }
    return nullptr;
}

void InspectorLabelColumn::Update()
{
    if (mIsUpdating == true)
    {
        return;
    }
    mIsUpdating = true;
    const QFontMetrics metrics = mRoot.fontMetrics();
    const int32 width = getAvailableWidth();
    const int32 minimum = metrics.averageCharWidth() * LABEL_MINIMUM_CHARACTERS;
    const int32 maximum = metrics.averageCharWidth() * LABEL_MAXIMUM_CHARACTERS;
    QList<QFormLayout*> forms;
    int32 widestField = GetReadableFieldWidth(metrics);
    for (QFormLayout* form : mRoot.findChildren<QFormLayout*>())
    {
        if (form->property(INSPECTOR_FORM_PROPERTY).toBool() == false)
        {
            continue;
        }
        forms.append(form);
        for (int32 row = 0; row < form->rowCount(); ++row)
        {
            const QLayoutItem* labelOrNull = form->itemAt(row, QFormLayout::LabelRole);
            const QLayoutItem* fieldOrNull = form->itemAt(row, QFormLayout::FieldRole);
            if (labelOrNull != nullptr && fieldOrNull != nullptr && fieldOrNull->isEmpty() == false)
            {
                widestField = std::max(widestField, fieldOrNull->minimumSize().width());
            }
        }
    }
    // The column gives way to the widest field, down to its minimum, before the labels move above their fields.
    const int32 roomForLabels = width - design::SPACING_SMALL - widestField;
    const int32 preferred = std::clamp(width * LABEL_COLUMN_PERCENT / PERCENT, minimum, maximum);
    mLabelWidth = std::max(std::min(preferred, roomForLabels), minimum);
    mIsStacked = width > 0 && roomForLabels < mLabelWidth;
    for (QFormLayout* form : forms)
    {
        applyTo(*form);
    }
    mIsUpdating = false;
}

bool InspectorLabelColumn::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == &mRoot || watched == mViewport.data()) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest ||
         event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange))
    {
        Update();
    }
    return QObject::eventFilter(watched, event);
}

void InspectorLabelColumn::applyTo(QFormLayout& inoutForm) const
{
    inoutForm.setContentsMargins(0, 0, 0, 0);
    inoutForm.setHorizontalSpacing(design::SPACING_SMALL);
    inoutForm.setVerticalSpacing(design::SPACING_SMALL);
    // Left-aligned labels read down one edge; macOS would otherwise right-align them against the fields.
    inoutForm.setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    inoutForm.setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    inoutForm.setRowWrapPolicy(mIsStacked == true ? QFormLayout::WrapAllRows : QFormLayout::DontWrapRows);
    for (int32 row = 0; row < inoutForm.rowCount(); ++row)
    {
        const QLayoutItem* labelItemOrNull = inoutForm.itemAt(row, QFormLayout::LabelRole);
        const QLayoutItem* fieldItemOrNull = inoutForm.itemAt(row, QFormLayout::FieldRole);
        QLabel* labelOrNull = labelItemOrNull != nullptr ? qobject_cast<QLabel*>(labelItemOrNull->widget()) : nullptr;
        if (labelOrNull == nullptr)
        {
            continue;
        }
        InspectorLabel* inspectorLabelOrNull = qobject_cast<InspectorLabel*>(labelOrNull);
        QCheckBox* checkOrNull =
            fieldItemOrNull != nullptr ? qobject_cast<QCheckBox*>(fieldItemOrNull->widget()) : nullptr;
        if (inspectorLabelOrNull != nullptr && checkOrNull != nullptr && inspectorLabelOrNull->buddy() == checkOrNull)
        {
            // Labels above their fields would leave a check box alone on its own line: it carries its label instead.
            const QString text = mIsStacked == true ? inspectorLabelOrNull->text() : QString();
            if (checkOrNull->text() != text)
            {
                checkOrNull->setText(text);
            }
            if (inoutForm.isRowVisible(row) == true && inspectorLabelOrNull->isHidden() != mIsStacked)
            {
                inspectorLabelOrNull->setVisible(mIsStacked == false);
            }
        }
        if (mIsStacked == true)
        {
            labelOrNull->setMinimumWidth(0);
            labelOrNull->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        else
        {
            labelOrNull->setFixedWidth(mLabelWidth);
            if (inspectorLabelOrNull == nullptr && labelOrNull->wordWrap() == false)
            {
                // A plain label cannot shorten itself; it wraps within the column rather than run under its field.
                labelOrNull->setWordWrap(true);
            }
        }
        const QPalette::ColorRole role = mIsStacked == true ? QPalette::PlaceholderText : QPalette::WindowText;
        if (labelOrNull->foregroundRole() != role)
        {
            labelOrNull->setForegroundRole(role);
        }
    }
}
} // namespace ludus::editor
