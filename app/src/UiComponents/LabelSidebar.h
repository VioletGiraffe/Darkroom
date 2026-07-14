#pragma once

#include "Core/LabelId.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class SegmentedToggle;
class Library;

// Left-hand label filter for the main window. Lists every label (color swatch + name + item count) plus an
// "All" row, lets the user toggle any number of them as an active filter, and offers an AND/OR combine mode.
// It owns only the filter UI and its state: the owner reads activeLabelIds()/isAndMode() (on filterChanged)
// to drive the grid, and calls refresh() after the catalog changes.
class LabelSidebar final : public QWidget
{
	Q_OBJECT
public:
	explicit LabelSidebar(Library& library, QWidget* parent = nullptr);

	// Rebuilds the rows from the Catalog (labels, order, counts) and drops any active filter ids that no
	// longer exist. Does not emit filterChanged.
	void refresh();

	[[nodiscard]] QList<LabelId> activeLabelIds() const;   // empty == no filter (the "All" row)
	[[nodiscard]] bool isAndMode() const;

	// Restores a persisted filter without emitting filterChanged (the caller refreshes the grid itself).
	void setActiveFilter(const QList<LabelId>& labelIds, bool andMode);

signals:
	void filterChanged();
	void addLabelRequested();
	// Per-label management, requested from a row's right-click menu. The owner (MainWindow) runs the dialog +
	// Catalog mutation and refreshes the view, mirroring how addLabelRequested is handled.
	void renameLabelRequested(LabelId labelId);
	void setLabelColorRequested(LabelId labelId);
	void deleteLabelRequested(LabelId labelId);

private:
	void rebuildRows();
	void applyRowHighlight();
	void onItemClicked(QListWidgetItem* item);
	void showRowContextMenu(const QPoint& pos);   // right-click a label row -> rename / set color / delete

private:
	Library&          m_library;
	QListWidget*      m_list        = nullptr;   // flat single-column list; rows painted by a custom delegate
	SegmentedToggle*  m_andOrToggle = nullptr;   // OR / AND combine-mode control (segment 1 == AND)
	QSet<LabelId>     m_activeLabelIds;   // empty == All (no filter)
};
