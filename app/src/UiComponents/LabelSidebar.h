#pragma once

#include "Core/LabelId.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QList>
#include <QSet>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

class QListWidget;
class QListWidgetItem;
class SegmentedToggle;
class Library;

// Main-window label filter with All, multi-label selection, and AND/OR combination.
class LabelSidebar final : public QWidget
{
	Q_OBJECT
public:
	explicit LabelSidebar(Library& library, QWidget* parent = nullptr);

	// Rebuilds rows, dropping stale active ids without emitting filterChanged.
	void refresh();

	[[nodiscard]] QList<LabelId> activeLabelIds() const;   // empty == All
	[[nodiscard]] bool isAndMode() const;

	// Restores state without emitting filterChanged.
	void setActiveFilter(const QList<LabelId>& labelIds, bool andMode);

signals:
	void filterChanged();
	void addLabelRequested();
	void renameLabelRequested(LabelId labelId);
	void setLabelColorRequested(LabelId labelId);
	void deleteLabelRequested(LabelId labelId);

private:
	void rebuildRows();
	void applyRowHighlight();
	void onItemClicked(QListWidgetItem* item);
	void showRowContextMenu(const QPoint& pos);

private:
	Library&          _library;
	QListWidget*      _list        = nullptr;
	SegmentedToggle*  _andOrToggle = nullptr;
	QSet<LabelId>     _activeLabelIds;
};
