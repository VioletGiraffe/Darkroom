#pragma once

#include <QListWidget>

#include <functional>

class QUrl;

// The MainWindow media-card grid: a plain icon-mode QListWidget whose one specialization is that dragging a
// card exports the underlying source file(s). Starting a drag on a card produces a file:// URL drop payload
// (CopyAction) for the current selection - so a multi-selection drags every selected file out to Explorer or
// another app at once - using the grabbed card as the drag image. The grid stays free of catalog/business
// logic: MainWindow supplies the item -> URL mapping via setDragUrlsProvider (the "MainWindow computes, card
// draws" split used for the cards themselves).
class MediaGrid final : public QListWidget {
public:
	using QListWidget::QListWidget;

	// Maps the dragged items to the source-file URLs to export (missing files skipped; an empty result cancels
	// the drag). Called by startDrag with the current selection.
	void setDragUrlsProvider(std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> provider);

protected:
	void startDrag(Qt::DropActions supportedActions) override;

private:
	std::function<QList<QUrl>(const QList<QListWidgetItem*>&)> m_dragUrlsProvider;
};
