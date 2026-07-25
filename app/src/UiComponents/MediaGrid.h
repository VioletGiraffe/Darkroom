#pragma once

#include <QListWidget>

#include <functional>

class QUrl;

// Icon-mode media grid that exports the selected source URLs with CopyAction and paints an empty message when
// no item is visible. MediaBrowserWidget supplies both data sources; the view contains no catalog logic.
class MediaGrid final : public QListWidget {
public:
	using QListWidget::QListWidget;

	// Called with the current selection; an empty result cancels the drag.
	void setDragUrlsProvider(std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> provider);

	void setEmptyMessage(const QString& message);

protected:
	void startDrag(Qt::DropActions supportedActions) override;
	void paintEvent(QPaintEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;

private:
	std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> _dragUrlsProvider;
	QString _emptyMessage;
};
