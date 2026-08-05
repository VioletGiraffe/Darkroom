#pragma once

#include <QListWidget>

#include <functional>

class QResizeEvent;
class QUrl;

// Icon-mode media grid that exports the selected source URLs with CopyAction and paints an empty message when
// no item is visible. MediaBrowserWidget supplies the data sources; the view contains no catalog logic.
class MediaGrid final : public QListWidget {
public:
	using QListWidget::QListWidget;

	// Called with the current selection; an empty result cancels the drag.
	void setDragUrlsProvider(std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> provider);

	// Rows carry no card until they approach the viewport; the owner builds one for the given row on demand.
	void setCardFactory(std::function<QWidget*(QListWidgetItem*)> factory);

	// Idempotent. Call after changing which rows exist, which are hidden, or where the view is scrolled to.
	void ensureVisibleCardsExist();

	// Drops every card; ensureVisibleCardsExist() rebuilds the ones still on screen.
	void discardAllCards();

	void setEmptyMessage(const QString& message);

protected:
	void startDrag(Qt::DropActions supportedActions) override;
	void paintEvent(QPaintEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	void scrollContentsBy(int dx, int dy) override;

private:
	std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> _dragUrlsProvider;
	std::function<QWidget*(QListWidgetItem*)> _cardFactory;
	QString _emptyMessage;
};
