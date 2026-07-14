#include "UiComponents/LabelSidebar.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "UiComponents/ContentWidthListWidget.h"
#include "UiComponents/DragGestureHelper.h"
#include "UiComponents/LabelMimeType.h"
#include "UiComponents/LabelRowDelegate.h"
#include "UiComponents/SegmentedToggle.h"
#include "Theme/Icons.h"
#include "Theme/Theme.h"
#include "Shortcuts.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHash>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

namespace {
constexpr int kLabelIdRole = Qt::UserRole;   // LabelId (QVariant::fromValue): the row's label; AllLabelId on the All row

// The All row carries no real label - None is distinct from Best and every generated id.
constexpr LabelId AllLabelId = LabelId::None;

}

LabelSidebar::LabelSidebar(Library& library, QWidget* parent) : QWidget(parent), m_library(library)
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

	// A flat single-column list used purely as a row container; LabelRowDelegate paints each row (color swatch,
	// name, right-aligned count, plus the active tint/spine highlight and the hover outline).
	// ContentWidthListWidget sizes the panel to the widest row, so names never need to clip.
	m_list = new ContentWidthListWidget();
	m_list->setItemDelegate(new LabelRowDelegate(m_list));
	m_list->setSelectionMode(QAbstractItemView::NoSelection);  // we manage active state ourselves (click toggles)
	m_list->setMouseTracking(true);   // so the delegate's hover highlight repaints as the cursor moves
	m_list->setMinimumWidth(140);

	// A press-and-drag on an ordinary label row drags the label out, to be dropped onto a card. "All" and the
	// virtual Best are filter-only, so they don't drag. Plain clicks are untouched: click-to-filter still works.
	new ListRowDragFilter(m_list, [](const QListWidgetItem* item) -> QMimeData* {
		const LabelId labelId = item->data(kLabelIdRole).value<LabelId>();
		if (labelId == AllLabelId || labelId == Catalog::BestLabelId)
			return nullptr;
		auto* mime = new QMimeData();
		mime->setData(LabelMimeType, toString(labelId).toUtf8());
		return mime;
	});
	layout->addWidget(m_list, 1);

	connect(m_list, &QListWidget::itemClicked, this, &LabelSidebar::onItemClicked);

	m_list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_list, &QListWidget::customContextMenuRequested, this, &LabelSidebar::showRowContextMenu);

	auto* btnCreateLabel = new QPushButton(tr("Create label"));
	btnCreateLabel->setObjectName("addLabelButton");
	btnCreateLabel->setIcon(Theme::tintedIcon(QStringLiteral(":/UI/icon_plus.svg"), &Theme::ThemeColors::TextPrimary));
	// Ctrl+L is mirrored on ImportDialog's Create-label button; keep the two in sync. The tooltip surfaces the
	// shortcut (derived from it, so there's a single source of truth) since a button doesn't advertise one otherwise.
	btnCreateLabel->setShortcut(QKeySequence(Shortcuts::CreateLabel));
	btnCreateLabel->setToolTip(tr("Create a new label (%1)").arg(btnCreateLabel->shortcut().toString(QKeySequence::NativeText)));
	layout->addWidget(btnCreateLabel);
	connect(btnCreateLabel, &QPushButton::clicked, this, &LabelSidebar::addLabelRequested);

	rebuildRows();
}

void LabelSidebar::refresh()
{
	rebuildRows();
}

void LabelSidebar::rebuildRows()
{
	m_list->clear();
	Catalog& catalog = m_library.catalog();
	const QHash<LabelId, int> counts = catalog.labelMediaItemCounts();
	const std::vector<Catalog::Label>& labels = catalog.allLabels();

	const auto addLabelRow = [&](const Catalog::Label& l, bool star) {
		auto* item = new QListWidgetItem(l.displayName, m_list);
		item->setData(kLabelIdRole, QVariant::fromValue(l.id));
		item->setData(LabelRowDelegate::CountRole, QString::number(counts.value(l.id, 0)));
		item->setData(LabelRowDelegate::SwatchColorRole, LabelRowDelegate::swatchColor(l.color));
		if (star)
			item->setData(LabelRowDelegate::StarRole, true);
	};

	// Pinned rows: All, then Best (gold swatch + star), separated from the ordinary labels by a divider.
	auto* allItem = new QListWidgetItem(tr("All"), m_list);
	allItem->setData(kLabelIdRole, QVariant::fromValue(AllLabelId));
	allItem->setData(LabelRowDelegate::AllRole, true);   // stack icon instead of a swatch
	allItem->setData(LabelRowDelegate::CountRole, QString::number(catalog.mediaItemCount()));

	for (const Catalog::Label& l : labels)
		if (l.id == Catalog::BestLabelId)
			addLabelRow(l, /*star*/ true);

	bool hasOrdinary = false;
	for (const Catalog::Label& l : labels)
		if (l.id != Catalog::BestLabelId) { hasOrdinary = true; break; }
	if (hasOrdinary)
	{
		auto* divider = new QListWidgetItem(m_list);
		divider->setData(LabelRowDelegate::DividerRole, true);
		divider->setFlags(Qt::NoItemFlags);   // not selectable, clickable or hoverable
	}

	for (const Catalog::Label& l : labels)
		if (l.id != Catalog::BestLabelId)
			addLabelRow(l, /*star*/ false);

	// Drop any active ids whose label no longer exists.
	QSet<LabelId> existing;
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
		if (item->data(LabelRowDelegate::DividerRole).toBool())
			continue;
		const LabelId id = item->data(kLabelIdRole).value<LabelId>();
		const bool active = id == AllLabelId ? m_activeLabelIds.isEmpty() : m_activeLabelIds.contains(id);
		item->setData(LabelRowDelegate::ActiveRole, active);
	}
	m_list->viewport()->update();   // repaint with the new active flags (the delegate reads ActiveRole)
}

void LabelSidebar::onItemClicked(QListWidgetItem* item)
{
	if (item->data(LabelRowDelegate::DividerRole).toBool())
		return;   // the separator isn't a filter (also disabled, so this shouldn't be called at all)
	const LabelId id = item->data(kLabelIdRole).value<LabelId>();
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

void LabelSidebar::showRowContextMenu(const QPoint& pos)
{
	const QListWidgetItem* item = m_list->itemAt(pos);
	if (!item)
		return;
	const LabelId labelId = item->data(kLabelIdRole).value<LabelId>();
	if (labelId == AllLabelId || labelId == Catalog::BestLabelId)
		return;  // "All" and the virtual Best aren't user-managed

	QMenu menu(this);
	menu.addAction(tr("Rename..."), this, [this, labelId] { emit renameLabelRequested(labelId); });
	menu.addAction(tr("Set color..."), this, [this, labelId] { emit setLabelColorRequested(labelId); });
	menu.addSeparator();
	menu.addAction(tr("Delete"), this, [this, labelId] { emit deleteLabelRequested(labelId); });
	menu.exec(m_list->viewport()->mapToGlobal(pos));
}

QList<LabelId> LabelSidebar::activeLabelIds() const
{
	return QList<LabelId>(m_activeLabelIds.cbegin(), m_activeLabelIds.cend());
}

bool LabelSidebar::isAndMode() const
{
	return m_andOrToggle->currentIndex() == 1;   // segment 1 == AND
}

void LabelSidebar::setActiveFilter(const QList<LabelId>& labelIds, bool andMode)
{
	m_activeLabelIds = QSet<LabelId>(labelIds.cbegin(), labelIds.cend());

	m_andOrToggle->setCurrentIndex(andMode ? 1 : 0);   // silent; the caller refreshes the grid

	applyRowHighlight();
}
