#include "UiComponents/LabelSidebar.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "UiComponents/ContentWidthListWidget.h"
#include "UiComponents/DragGestureHelper.h"
#include "UiComponents/LabelMimeType.h"
#include "UiComponents/LabelRowDelegate.h"
#include "UiComponents/SegmentedToggle.h"
#include "Theme/Theme.h"
#include "theme/ctintedsvgiconengine.h"
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
constexpr int kLabelIdRole = Qt::UserRole;

// The All row carries no real label - None is distinct from Best and every generated id.
constexpr LabelId AllLabelId = LabelId::None;

}

LabelSidebar::LabelSidebar(Library& library, QWidget* parent) : QWidget(parent), _library(library)
{
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	auto* header = new QHBoxLayout();
	header->addWidget(new QLabel(tr("Labels")));
	header->addStretch(1);
	_andOrToggle = new SegmentedToggle({ tr("OR"), tr("AND") });
	_andOrToggle->setToolTip(tr("Combine selected labels with OR (any) or AND (all)"));
	header->addWidget(_andOrToggle);
	layout->addLayout(header);

	connect(_andOrToggle, &SegmentedToggle::currentChanged, this, [this] {
		if (_activeLabelIds.size() > 1)
			emit filterChanged();
	});

	_list = new ContentWidthListWidget();
	_list->setItemDelegate(new LabelRowDelegate(_list));
	_list->setSelectionMode(QAbstractItemView::NoSelection);
	_list->setMouseTracking(true);
	_list->setMinimumWidth(140);

	new ListRowDragFilter(_list, [](const QListWidgetItem* item) -> QMimeData* {
		const LabelId labelId = item->data(kLabelIdRole).value<LabelId>();
		if (labelId == AllLabelId || labelId == Catalog::BestLabelId)
			return nullptr;
		auto* mime = new QMimeData();
		mime->setData(LabelMimeType, toString(labelId).toUtf8());
		return mime;
	});
	layout->addWidget(_list, 1);

	connect(_list, &QListWidget::itemClicked, this, &LabelSidebar::onItemClicked);

	_list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(_list, &QListWidget::customContextMenuRequested, this, &LabelSidebar::showRowContextMenu);

	auto* btnCreateLabel = new QPushButton(tr("Create label"));
	btnCreateLabel->setObjectName("addLabelButton");
	btnCreateLabel->setIcon(tintedSvgIcon(QStringLiteral(":/UI/icon_plus.svg"), [] { return Theme::current().palette.text; }));
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
	_list->clear();
	Catalog& catalog = _library.catalog();
	const QHash<LabelId, int> counts = catalog.labelMediaItemCounts();
	const std::vector<Catalog::Label>& labels = catalog.allLabels();

	const auto addLabelRow = [&](const Catalog::Label& l, bool star) {
		auto* item = new QListWidgetItem(l.displayName, _list);
		item->setData(kLabelIdRole, QVariant::fromValue(l.id));
		item->setData(LabelRowDelegate::CountRole, QString::number(counts.value(l.id, 0)));
		item->setData(LabelRowDelegate::SwatchColorRole, LabelRowDelegate::swatchColor(l.color));
		if (star)
			item->setData(LabelRowDelegate::StarRole, true);
	};

	auto* allItem = new QListWidgetItem(tr("All"), _list);
	allItem->setData(kLabelIdRole, QVariant::fromValue(AllLabelId));
	allItem->setData(LabelRowDelegate::AllRole, true);
	allItem->setData(LabelRowDelegate::CountRole, QString::number(catalog.mediaItemCount()));

	for (const Catalog::Label& l : labels)
		if (l.id == Catalog::BestLabelId)
			addLabelRow(l, /*star*/ true);

	bool hasOrdinary = false;
	for (const Catalog::Label& l : labels)
		if (l.id != Catalog::BestLabelId) { hasOrdinary = true; break; }
	if (hasOrdinary)
	{
		auto* divider = new QListWidgetItem(_list);
		divider->setData(LabelRowDelegate::DividerRole, true);
		divider->setFlags(Qt::NoItemFlags);
	}

	for (const Catalog::Label& l : labels)
		if (l.id != Catalog::BestLabelId)
			addLabelRow(l, /*star*/ false);

	QSet<LabelId> existing;
	for (const Catalog::Label& l : labels)
		existing.insert(l.id);
	_activeLabelIds.intersect(existing);

	applyRowHighlight();
	_list->updateGeometry();
}

void LabelSidebar::applyRowHighlight()
{
	for (int i = 0; i < _list->count(); ++i)
	{
		QListWidgetItem* item = _list->item(i);
		if (item->data(LabelRowDelegate::DividerRole).toBool())
			continue;
		const LabelId id = item->data(kLabelIdRole).value<LabelId>();
		const bool active = id == AllLabelId ? _activeLabelIds.isEmpty() : _activeLabelIds.contains(id);
		item->setData(LabelRowDelegate::ActiveRole, active);
	}
	_list->viewport()->update();
}

void LabelSidebar::onItemClicked(QListWidgetItem* item)
{
	if (item->data(LabelRowDelegate::DividerRole).toBool())
		return;
	const LabelId id = item->data(kLabelIdRole).value<LabelId>();
	if (id == AllLabelId)
	{
		if (_activeLabelIds.empty())
			return;

		_activeLabelIds.clear();
	}
	else if (!_activeLabelIds.remove(id))
		_activeLabelIds.insert(id);

	applyRowHighlight();
	emit filterChanged();
}

void LabelSidebar::showRowContextMenu(const QPoint& pos)
{
	const QListWidgetItem* item = _list->itemAt(pos);
	if (!item)
		return;
	const LabelId labelId = item->data(kLabelIdRole).value<LabelId>();
	if (labelId == AllLabelId || labelId == Catalog::BestLabelId)
		return;

	QMenu menu(this);
	menu.addAction(tr("Rename..."), this, [this, labelId] { emit renameLabelRequested(labelId); });
	menu.addAction(tr("Set color..."), this, [this, labelId] { emit setLabelColorRequested(labelId); });
	menu.addSeparator();
	menu.addAction(tr("Delete"), this, [this, labelId] { emit deleteLabelRequested(labelId); });
	menu.exec(_list->viewport()->mapToGlobal(pos));
}

QList<LabelId> LabelSidebar::activeLabelIds() const
{
	return QList<LabelId>(_activeLabelIds.cbegin(), _activeLabelIds.cend());
}

bool LabelSidebar::isAndMode() const
{
	return _andOrToggle->currentIndex() == 1;
}

void LabelSidebar::setActiveFilter(const QList<LabelId>& labelIds, bool andMode)
{
	_activeLabelIds = QSet<LabelId>(labelIds.cbegin(), labelIds.cend());

	_andOrToggle->setCurrentIndex(andMode ? 1 : 0);

	applyRowHighlight();
}
