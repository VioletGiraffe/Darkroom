#include "UiComponents/LabelSidebar.h"
#include "Core/Catalog.h"
#include "UiComponents/LabelMimeType.h"
#include "UiComponents/SegmentedToggle.h"
#include "Theme/Theme.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
constexpr int kLabelIdRole     = Qt::UserRole;       // QString: label id ("" == the All row)
constexpr int kCountRole       = Qt::UserRole + 1;   // QString: the per-row video count, painted right-aligned
constexpr int kSwatchColorRole = Qt::UserRole + 2;   // QColor: leading dot color (invalid == no dot, e.g. the All row)
constexpr int kActiveRole      = Qt::UserRole + 3;   // bool: row is in the active filter (painted highlighted)
constexpr int kStarRole        = Qt::UserRole + 4;   // bool: draw a gold star after the dot (the Best row)
constexpr int kDividerRole     = Qt::UserRole + 5;   // bool: separator line between the pinned rows and the labels

const QString AllLabelId = "7132b82a17dc479c876bCE651EA254F9";

inline QColor colorFromHex(const char* hex) { return QColor(QString::fromLatin1(hex)); }

inline QColor swatchColorFor(const Catalog::Label& l)
{
	if (!l.color.isEmpty())
		return QColor(l.color);
	return colorFromHex(Theme::current().MutedText);     // neutral for an uncolored label
}

// Paints a whole label row as one unit: a leading color dot, the name, a right-aligned count, and - for
// rows in the active filter - a soft highlight pill with a left accent bar tinted to that label's own color
// (the per-row accent color is exactly what plain QSS can't express, which is why this is a delegate).
// Hovered rows get a dashed outline instead. Two special rows are also handled: the Best row draws a gold
// star after its dot (kStarRole), and a kDividerRole row draws the separator between the pinned All/Best
// rows and the ordinary labels.
class LabelRowDelegate final : public QStyledItemDelegate {
private:
	static constexpr int DOT = 10;  // leading color dot diameter
	static constexpr int GAP = 8;   // dot -> name gap
	static constexpr int PAD_L = 10;  // left breathing room
	static constexpr int PAD_R = 12;  // right breathing room (count -> edge)
	static constexpr int PAD_V = 6;   // vertical breathing room per row
	static constexpr int COUNT_GAP = 16;  // minimum name -> count gap
	static constexpr int MARGIN = 2;   // inset of the highlight pill from the row edges
	static constexpr int RADIUS = Theme::ControlRadius;   // highlight pill corner radius
	static constexpr int ACCENT_W = 3;   // left accent bar width
	static constexpr int DIVIDER_H = 12;   // height of a separator row

public:
	using QStyledItemDelegate::QStyledItemDelegate;

	QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
	{
		if (index.data(kDividerRole).toBool())
			return { 0, DIVIDER_H };   // width 0 so the separator never drives the panel's hugged width

		const QString name  = index.data(Qt::DisplayRole).toString();
		const QString count = index.data(kCountRole).toString();
		const int starW = index.data(kStarRole).toBool() ? option.fontMetrics.horizontalAdvance(QStringLiteral("★")) + GAP : 0;
		const int width = PAD_L + DOT + GAP + starW + option.fontMetrics.horizontalAdvance(name)
		                + COUNT_GAP + option.fontMetrics.horizontalAdvance(count) + PAD_R;
		return { width, qMax(DOT, option.fontMetrics.height()) + 2 * PAD_V };
	}

	void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
	{
		p->save();
		p->setRenderHint(QPainter::Antialiasing);

		const QRect r = option.rect;
		const Theme::ThemeColors& t = Theme::current();

		if (index.data(kDividerRole).toBool())   // separator between the pinned rows and the labels
		{
			p->setPen(QPen(colorFromHex(t.BorderSubtle), 1.0));
			const int dy = r.bottom() - 5;
			p->drawLine(r.left() + PAD_L, dy, r.right() - PAD_R, dy);
			p->restore();
			return;
		}

		const QString name   = index.data(Qt::DisplayRole).toString();
		const QString count  = index.data(kCountRole).toString();
		const QColor  swatch = index.data(kSwatchColorRole).value<QColor>();
		const bool    active = index.data(kActiveRole).toBool();
		const bool    hover  = option.state & QStyle::State_MouseOver;

		// Active and hover get distinct shapes: active a soft filled pill + accent bar, hover a dotted outline.
		// Both colors are the text color as a translucent overlay, so they read against any background.
		const QRect pill = r.adjusted(MARGIN, 1, -MARGIN, -1);
		if (active)
		{
			QColor bg = option.palette.color(QPalette::Text);
			bg.setAlpha(36);
			p->setPen(Qt::NoPen);
			p->setBrush(bg);
			p->drawRoundedRect(pill, RADIUS, RADIUS);

			// Left accent bar in the label's own color (a fixed accent for the All row).
			const QColor accent = swatch.isValid() ? swatch : colorFromHex(t.AccentBorder);
			p->setBrush(accent);
			p->drawRoundedRect(QRectF(pill.left(), pill.top() + 2, ACCENT_W, pill.height() - 4), ACCENT_W / 2.0, ACCENT_W / 2.0);
		}

		if (hover)
		{
			QColor line = option.palette.color(QPalette::Text);
			line.setAlpha(120);
			QPen pen(line);
			pen.setStyle(Qt::DashLine);
			p->setPen(pen);
			p->setBrush(Qt::NoBrush);
			p->drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), RADIUS, RADIUS);   // half-px inset keeps the 1px stroke crisp
		}

		const int cy   = r.center().y();
		const int dotX = r.left() + PAD_L;
		if (swatch.isValid())
		{
			p->setPen(Qt::NoPen);
			p->setBrush(swatch);
			p->drawEllipse(QPointF(dotX + DOT / 2.0, cy), DOT / 2.0, DOT / 2.0);
		}

		int nameX = dotX + DOT + GAP;
		if (index.data(kStarRole).toBool())   // the Best row: a gold star just right of its dot
		{
			const QString star  = QStringLiteral("★");
			const int     starW = option.fontMetrics.horizontalAdvance(star);
			p->setPen(colorFromHex(Theme::StarActive));
			p->drawText(QRect(nameX, r.top(), starW, r.height()), Qt::AlignLeft | Qt::AlignVCenter, star);
			nameX += starW + GAP;
		}

		// Count rides the right edge; the name fills (and elides into) the space up to it.
		const int   countW = option.fontMetrics.horizontalAdvance(count);
		const QRect countRect(r.right() - PAD_R - countW, r.top(), countW, r.height());
		p->setPen(colorFromHex(t.MutedText));
		p->drawText(countRect, Qt::AlignRight | Qt::AlignVCenter, count);

		const QRect nameRect(nameX, r.top(), countRect.left() - COUNT_GAP - nameX, r.height());
		p->setPen(option.palette.color(QPalette::Text));
		p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, option.fontMetrics.elidedText(name, Qt::ElideRight, nameRect.width()));

		p->restore();
	}
};

// A QListWidget that reports the width its widest row actually needs (plus frame and, when shown, the
// scrollbar), so the sidebar layout/splitter sizes the panel to hug its content instead of QListView's
// large default. Height keeps the base behavior. The breathing room is folded into the delegate's sizeHint.
class ContentWidthListWidget final : public QListWidget {
public:
	using QListWidget::QListWidget;

	[[nodiscard]] QSize sizeHint() const override        { return { contentWidth(), QListWidget::sizeHint().height() }; }
	[[nodiscard]] QSize minimumSizeHint() const override { return { contentWidth(), QListWidget::minimumSizeHint().height() }; }

private:
	[[nodiscard]] int contentWidth() const
	{
		int width = 2 * frameWidth() + qMax(sizeHintForColumn(0), 0);   // widest row via the delegate; -1 when empty
		if (verticalScrollBar()->isVisible())
			width += verticalScrollBar()->sizeHint().width();
		return width;
	}
};
}

LabelSidebar::LabelSidebar(QWidget* parent) : QWidget(parent)
{
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	auto* header = new QHBoxLayout();
	header->addWidget(new QLabel(tr("Labels")));
	header->addStretch(1);
	m_andOrToggle = new SegmentedToggle({ tr("OR"), tr("AND") });
	m_andOrToggle->setToolTip(tr("Combine selected labels with OR (any) or AND (all)"));
	header->addWidget(m_andOrToggle);
	layout->addLayout(header);

	connect(m_andOrToggle, &SegmentedToggle::currentChanged, this, [this] {
		// Only emit when more than 1 filter is selected, otherwise there is no actual change
		if (m_activeLabelIds.size() > 1)
			emit filterChanged();
	});

	// A flat single-column list used purely as a row container; LabelRowDelegate paints each row (color dot,
	// name, right-aligned count, plus the active/hover highlight and accent bar). ContentWidthListWidget
	// sizes the panel to the widest row, so names never need to clip.
	m_list = new ContentWidthListWidget();
	m_list->setItemDelegate(new LabelRowDelegate(m_list));
	m_list->setSelectionMode(QAbstractItemView::NoSelection);  // we manage active state ourselves (click toggles)
	m_list->setMouseTracking(true);   // so the delegate's hover highlight repaints as the cursor moves
	m_list->setMinimumWidth(140);

	m_list->viewport()->installEventFilter(this);  // drives the row-drag gesture (see eventFilter)
	layout->addWidget(m_list, 1);

	connect(m_list, &QListWidget::itemClicked, this, &LabelSidebar::onItemClicked);

	m_list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_list, &QListWidget::customContextMenuRequested, this, &LabelSidebar::showRowContextMenu);

	auto* addButton = new QPushButton(tr("+ Add label"));
	addButton->setObjectName("addLabelButton");
	layout->addWidget(addButton);
	connect(addButton, &QPushButton::clicked, this, &LabelSidebar::addLabelRequested);

	rebuildRows();
}

void LabelSidebar::refresh()
{
	rebuildRows();
}

void LabelSidebar::rebuildRows()
{
	m_list->clear();
	Catalog& catalog = Catalog::instance();
	const QHash<QString, int> counts = catalog.labelVideoCounts();
	const QList<Catalog::Label> labels = catalog.allLabels();

	const auto addLabelRow = [&](const Catalog::Label& l, bool star) {
		auto* item = new QListWidgetItem(l.displayName, m_list);
		item->setData(kLabelIdRole, l.id);
		item->setData(kCountRole, QString::number(counts.value(l.id, 0)));
		item->setData(kSwatchColorRole, swatchColorFor(l));
		if (star)
			item->setData(kStarRole, true);
	};

	// Pinned rows: All, then Best (gold dot + star), separated from the ordinary labels by a divider.
	auto* allItem = new QListWidgetItem(tr("All"), m_list);
	allItem->setData(kLabelIdRole, AllLabelId);
	allItem->setData(kCountRole, QString::number(catalog.videoCount()));

	for (const Catalog::Label& l : labels)
		if (l.id == Catalog::BestLabelId)
			addLabelRow(l, /*star*/ true);

	bool hasOrdinary = false;
	for (const Catalog::Label& l : labels)
		if (l.id != Catalog::BestLabelId) { hasOrdinary = true; break; }
	if (hasOrdinary)
	{
		auto* divider = new QListWidgetItem(m_list);
		divider->setData(kDividerRole, true);
		divider->setFlags(Qt::NoItemFlags);   // not selectable, clickable or hoverable
	}

	for (const Catalog::Label& l : labels)
		if (l.id != Catalog::BestLabelId)
			addLabelRow(l, /*star*/ false);

	// Drop any active ids whose label no longer exists.
	QSet<QString> existing;
	for (const Catalog::Label& l : labels)
		existing.insert(l.id);
	m_activeLabelIds.intersect(existing);

	applyRowHighlight();
	m_list->updateGeometry();  // row set/text changed the content width; let the layout/splitter re-fit
}

void LabelSidebar::applyRowHighlight()
{
	for (int i = 0; i < m_list->count(); ++i)
	{
		QListWidgetItem* item = m_list->item(i);
		if (item->data(kDividerRole).toBool())
			continue;
		const QString id = item->data(kLabelIdRole).toString();
		const bool active = id == AllLabelId ? m_activeLabelIds.isEmpty() : m_activeLabelIds.contains(id);
		item->setData(kActiveRole, active);
	}
	m_list->viewport()->update();   // repaint with the new active flags (the delegate reads kActiveRole)
}

void LabelSidebar::onItemClicked(QListWidgetItem* item)
{
	if (item->data(kDividerRole).toBool())
		return;   // the separator isn't a filter (also disabled, so this shouldn't be called at all)
	const QString id = item->data(kLabelIdRole).toString();
	if (id == AllLabelId)
	{
		// If 'All' is already applied - don't emit a change (there was none)
		if (m_activeLabelIds.empty())
			return;

		m_activeLabelIds.clear();           // "All" clears the filter
	}
	else if (!m_activeLabelIds.remove(id))
		m_activeLabelIds.insert(id);        // toggle this label in/out of the filter

	applyRowHighlight();
	emit filterChanged();
}

bool LabelSidebar::eventFilter(QObject* watched, QEvent* event)
{
	if (watched != m_list->viewport())
		return QWidget::eventFilter(watched, event);

	switch (event->type())
	{
	case QEvent::MouseButtonPress:
	{
		auto* me = static_cast<QMouseEvent*>(event);
		m_dragHelper.mousePressed(me);
		m_pressedItem = m_list->itemAt(me->pos());
		break;
	}
	case QEvent::MouseMove:
	{
		if (!m_pressedItem)
			break;
		const QString labelId = m_pressedItem->data(kLabelIdRole).toString();
		// Only ordinary labels are draggable to assign; "All" (empty id) and the virtual Best are filter-only.
		if (labelId == AllLabelId || labelId == Catalog::BestLabelId)
			break;

		auto* me = static_cast<QMouseEvent*>(event);
		const QPixmap rowPixmap = m_list->viewport()->grab(m_list->visualItemRect(m_pressedItem));
		const bool started = m_dragHelper.tryStartDrag(
			m_list->viewport(), me,
			[labelId] {
				auto* mime = new QMimeData();
				mime->setData(LabelMimeType, labelId.toUtf8());
				return mime;
			},
			Qt::CopyAction, rowPixmap);
		if (started)
		{
			m_pressedItem = nullptr;
			return true;
		}
		break;
	}
	case QEvent::MouseButtonRelease:
		m_pressedItem = nullptr;
		break;
	default:
		break;
	}

	return QWidget::eventFilter(watched, event);
}

void LabelSidebar::showRowContextMenu(const QPoint& pos)
{
	const QListWidgetItem* item = m_list->itemAt(pos);
	if (!item)
		return;
	const QString labelId = item->data(kLabelIdRole).toString();
	if (labelId == AllLabelId || labelId == Catalog::BestLabelId)
		return;  // "All" and the virtual Best aren't user-managed

	QMenu menu(this);
	menu.addAction(tr("Rename..."), this, [this, labelId] { emit renameLabelRequested(labelId); });
	menu.addAction(tr("Set color..."), this, [this, labelId] { emit setLabelColorRequested(labelId); });
	menu.addSeparator();
	menu.addAction(tr("Delete"), this, [this, labelId] { emit deleteLabelRequested(labelId); });
	menu.exec(m_list->viewport()->mapToGlobal(pos));
}

QStringList LabelSidebar::activeLabelIds() const
{
	return QStringList(m_activeLabelIds.cbegin(), m_activeLabelIds.cend());
}

bool LabelSidebar::isAndMode() const
{
	return m_andOrToggle->currentIndex() == 1;   // segment 1 == AND
}

void LabelSidebar::setActiveFilter(const QStringList& labelIds, bool andMode)
{
	m_activeLabelIds = QSet<QString>(labelIds.cbegin(), labelIds.cend());

	m_andOrToggle->setCurrentIndex(andMode ? 1 : 0);   // silent; the caller refreshes the grid

	applyRowHighlight();
}
