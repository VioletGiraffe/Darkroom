#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QListWidget>
RESTORE_COMPILER_WARNINGS

#include <functional>

class QItemSelection;
class QResizeEvent;
class QUrl;

// Icon-mode media grid that exports the selected source URLs by drag and paints an empty message when
// no item is visible. Its owner supplies the data sources; the view contains no catalog logic.
// Every card carries a "selected" dynamic property mirroring its row, so the central sheet can style the selected state.
// Hidden rows are never selected, so actions on the selection never reach them.
// A width change keeps the first visible item at its viewport offset through the reflow.
class MediaGrid final : public QListWidget {
public:
	explicit MediaGrid(QWidget* parent = nullptr);

	// Called with the current selection; an empty result cancels the drag.
	void setDragUrlsProvider(std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> provider);
	// The actions a drag offers its target; CopyAction unless set. The grid never deletes anything after a move.
	void setDragActions(Qt::DropActions actions);
	// Called once a drag ends, with the action the target reported and the URLs dragged.
	void setDragFinishedHandler(std::function<void(Qt::DropAction action, const QList<QUrl>& urls)> handler);

	// Rows carry no card until they approach the viewport; the owner builds one for the given row on demand.
	void setCardFactory(std::function<QWidget*(QListWidgetItem*)> factory);

	// Idempotent. Call after changing which rows exist, which are hidden, or where the view is scrolled to.
	void ensureVisibleCardsExist();

	// Drops every card; ensureVisibleCardsExist() rebuilds the ones still on screen.
	void discardAllCards();

	// Hides the rows isShown rejects and shows the rest. A hidden current row stops being current.
	void filterRows(const std::function<bool(const QListWidgetItem&)>& isShown);

	void setEmptyMessage(const QString& message);

protected:
	void startDrag(Qt::DropActions supportedActions) override;
	void paintEvent(QPaintEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	void scrollContentsBy(int dx, int dy) override;
	void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) override;

private:
	std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> _dragUrlsProvider;
	std::function<void(Qt::DropAction action, const QList<QUrl>& urls)> _dragFinishedHandler;
	Qt::DropActions _dragActions = Qt::CopyAction;
	std::function<QWidget*(QListWidgetItem*)> _cardFactory;
	QString _emptyMessage;
};
