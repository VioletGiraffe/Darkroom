#pragma once

#include <QListWidget>

#include <functional>

class QUrl;

// The MainWindow media-card grid: a plain icon-mode QListWidget with two specializations. Dragging a card
// exports the underlying source file(s): starting a drag produces a file:// URL drop payload (CopyAction)
// for the current selection - so a multi-selection drags every selected file out to Explorer or another app
// at once - using the grabbed card as the drag image. And when no card is visible (none built, or the name
// filter hid them all), a caller-set message paints centered over the viewport instead of a bare void. The
// grid stays free of catalog/business logic: MainWindow supplies the item -> URL mapping via
// setDragUrlsProvider and the empty-state wording via setEmptyMessage (the "MainWindow computes, card
// draws" split used for the cards themselves).
class MediaGrid final : public QListWidget {
public:
	using QListWidget::QListWidget;

	// Maps the dragged items to the source-file URLs to export (missing files skipped; an empty result cancels
	// the drag). Called by startDrag with the current selection.
	void setDragUrlsProvider(std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> provider);

	// The message painted over the viewport while no item is visible. Empty string disables the paint.
	void setEmptyMessage(const QString& message);

protected:
	void startDrag(Qt::DropActions supportedActions) override;
	void paintEvent(QPaintEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;

private:
	std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> m_dragUrlsProvider;
	QString m_emptyMessage;
};
