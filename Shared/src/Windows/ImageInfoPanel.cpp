#include "Windows/ImageInfoPanel.h"
#include "Core/IoThreadPool.h"
#include "Windows/ImageInfoRows.h"

DISABLE_COMPILER_WARNINGS
#include <QAbstractItemView>
#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <memory>
#include <utility>

namespace {

constexpr int FilterRole = Qt::UserRole;        // a row's name, value and name tooltip, matched by the filter
constexpr int FullValueRole = Qt::UserRole + 1; // the value before it is joined into one line
constexpr qsizetype MaxToolTipLength = 2000;

QString toolTipText(const QString& text)
{
	return text.size() > MaxToolTipLength ? text.first(MaxToolTipLength) + QStringLiteral("...") : text;
}

// Shows a value's full text as its tooltip when its cell elides it or joins its lines
class ValueToolTipDelegate final : public QStyledItemDelegate
{
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option, const QModelIndex& index) override
	{
		if (event->type() != QEvent::ToolTip || index.column() != 1)
			return QStyledItemDelegate::helpEvent(event, view, option, index);

		QStyleOptionViewItem textOption = option;
		initStyleOption(&textOption, index);
		const QStyle* style = view->style();
		// The margin QCommonStyle draws item text within
		const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, view) + 1;
		const int textWidth = style->subElementRect(QStyle::SE_ItemViewItemText, &textOption, view).width() - 2 * margin;
		const QString fullValue = index.data(FullValueRole).toString();
		if (fullValue == textOption.text && textOption.fontMetrics.horizontalAdvance(fullValue) <= textWidth)
		{
			QToolTip::hideText();
			return true;
		}

		QToolTip::showText(event->globalPos(), toolTipText(fullValue), view);
		return true;
	}
};

} // namespace

ImageInfoPanel::ImageInfoPanel(QWidget* parent)
	: QWidget{ parent }
{
	_filter = new QLineEdit{ this };
	_filter->setPlaceholderText(tr("Filter"));
	_filter->setClearButtonEnabled(true);
	_filter->installEventFilter(this);
	connect(_filter, &QLineEdit::textChanged, this, &ImageInfoPanel::applyFilter);

	_model = new QStandardItemModel{ this };
	_model->setHorizontalHeaderLabels({ tr("Name"), tr("Value") });
	_proxy = new QSortFilterProxyModel{ this };
	_proxy->setSourceModel(_model);
	_proxy->setFilterRole(FilterRole);
	_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
	_proxy->setRecursiveFilteringEnabled(true);
	_proxy->setAutoAcceptChildRows(true);

	_tree = new QTreeView{ this };
	_tree->setModel(_proxy);
	_tree->setItemDelegate(new ValueToolTipDelegate{ _tree });
	_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
	_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
	_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
	_tree->setUniformRowHeights(true);
	_tree->setAlternatingRowColors(true);
	_tree->header()->setStretchLastSection(true);
	_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	_tree->installEventFilter(this);
	connect(_tree, &QWidget::customContextMenuRequested, this, &ImageInfoPanel::showContextMenu);
	// Only the user's own collapsing is remembered: the filter expands every group
	connect(_tree, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
		if (_filter->text().isEmpty() && !index.parent().isValid())
			_collapsedGroups.insert(index.data().toString());
	});
	connect(_tree, &QTreeView::expanded, this, [this](const QModelIndex& index) {
		if (_filter->text().isEmpty() && !index.parent().isValid())
			_collapsedGroups.erase(index.data().toString());
	});

	auto* layout = new QVBoxLayout{ this };
	layout->setContentsMargins({});
	layout->addWidget(_filter);
	layout->addWidget(_tree);
}

void ImageInfoPanel::setPath(const QString& path)
{
	_path = path;
	readUnlessRequested();
}

void ImageInfoPanel::focusFilter()
{
	_filter->setFocus(Qt::ShortcutFocusReason);
	_filter->selectAll();
}

bool ImageInfoPanel::eventFilter(QObject* watched, QEvent* event)
{
	if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
		return QWidget::eventFilter(watched, event);

	// A ShortcutOverride accepted here keeps the window's shortcut from taking the key press that follows
	const auto* key = static_cast<QKeyEvent*>(event);
	if (watched == _filter && key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier && !_filter->text().isEmpty())
	{
		event->accept();
		if (event->type() == QEvent::KeyPress)
			_filter->clear();
		return true;
	}
	if (watched == _tree && key->matches(QKeySequence::Copy))
	{
		event->accept();
		if (event->type() == QEvent::KeyPress)
			QGuiApplication::clipboard()->setText(rowsText(true));
		return true;
	}
	return QWidget::eventFilter(watched, event);
}

void ImageInfoPanel::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	readUnlessRequested();
}

void ImageInfoPanel::wheelEvent(QWheelEvent* event)
{
	event->accept();
}

void ImageInfoPanel::readUnlessRequested()
{
	if (!isVisible() || _path.isEmpty() || _path == _requestedPath)
		return;

	_requestedPath = _path;
	auto delivery = std::make_shared<AsyncDelivery<ImageMetadata::ImageInfo>>(this, [this, path = _path](ImageMetadata::ImageInfo info) {
		showInfo(path, info);
	});
	_read = AsyncRequest{ delivery };
	IoThreadPool::enqueue(_path, [delivery, path = _path] {
		if (!delivery->isCancelled())
			delivery->deliver(ImageMetadata::readImageInfo(path), delivery);
	});
}

void ImageInfoPanel::showInfo(const QString& path, const ImageMetadata::ImageInfo& info)
{
	_model->removeRows(0, _model->rowCount());
	for (const ImageInfoRows::Group& group : ImageInfoRows::groups(path, info))
	{
		auto* groupItem = new QStandardItem{ group.title };
		groupItem->setData(group.title, FilterRole);
		QFont titleFont = groupItem->font();
		titleFont.setBold(true);
		groupItem->setFont(titleFont);

		for (const ImageInfoRows::Row& row : group.rows)
		{
			auto* name = new QStandardItem{ row.key };
			name->setToolTip(row.keyToolTip);
			name->setData(QStringList{ row.key, row.value, row.keyToolTip }.join(u'\n'), FilterRole);
			auto* value = new QStandardItem{ row.value.simplified() };
			value->setData(row.value, FullValueRole);
			groupItem->appendRow({ name, value });
		}
		_model->appendRow({ groupItem, new QStandardItem });
	}

	spanGroupRows();
	expandGroups();
	if (!_nameColumnSized)
	{
		_nameColumnSized = true;
		_tree->resizeColumnToContents(0);
		_tree->setColumnWidth(0, std::min(_tree->columnWidth(0), _tree->viewport()->width() / 2));
	}
}

void ImageInfoPanel::applyFilter(const QString& text)
{
	_proxy->setFilterFixedString(text);
	spanGroupRows();
	expandGroups();
}

void ImageInfoPanel::expandGroups()
{
	const bool filtering = !_filter->text().isEmpty();
	for (int row = 0; row < _proxy->rowCount(); ++row)
	{
		const QModelIndex group = _proxy->index(row, 0);
		_tree->setExpanded(group, filtering || !_collapsedGroups.contains(group.data().toString()));
	}
}

void ImageInfoPanel::spanGroupRows()
{
	for (int row = 0; row < _proxy->rowCount(); ++row)
		_tree->setFirstColumnSpanned(row, {}, true);
}

void ImageInfoPanel::showContextMenu(QPoint position)
{
	const QModelIndex index = _tree->indexAt(position);
	QMenu menu;
	QAction* copyAction = menu.addAction(tr("Copy"), this, [this] { QGuiApplication::clipboard()->setText(rowsText(true)); });
	copyAction->setShortcut(QKeySequence::Copy);
	copyAction->setEnabled(_tree->selectionModel()->hasSelection());
	if (index.isValid() && index.parent().isValid())
	{
		// Persistent: a read finishing while the menu is open replaces the rows
		menu.addAction(tr("Copy value"), this, [value = QPersistentModelIndex{ index.siblingAtColumn(1) }] {
			if (value.isValid())
				QGuiApplication::clipboard()->setText(value.data(FullValueRole).toString());
		});
	}
	menu.addAction(tr("Copy all"), this, [this] { QGuiApplication::clipboard()->setText(rowsText(false)); });
	menu.exec(_tree->viewport()->mapToGlobal(position));
}

QString ImageInfoPanel::rowsText(bool selectedOnly) const
{
	const QItemSelectionModel* selection = _tree->selectionModel();
	QStringList lines;
	for (int groupRow = 0; groupRow < _proxy->rowCount(); ++groupRow)
	{
		const QModelIndex group = _proxy->index(groupRow, 0);
		if (!selectedOnly || selection->isRowSelected(groupRow, {}))
			lines.push_back(group.data().toString());

		for (int row = 0; row < _proxy->rowCount(group); ++row)
		{
			if (!selectedOnly || selection->isRowSelected(row, group))
				lines.push_back(_proxy->index(row, 0, group).data().toString() + u'\t' + _proxy->index(row, 1, group).data(FullValueRole).toString());
		}
	}
	return lines.join(u'\n');
}
