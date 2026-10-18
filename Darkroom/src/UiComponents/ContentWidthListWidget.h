#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QEvent>
#include <QListWidget>
#include <QScrollBar>
#include <QSize>
RESTORE_COMPILER_WARNINGS

// A QListWidget whose width hugs its widest delegated row plus frame and visible scrollbar. Callers bound
// pathological rows with setMaximumWidth() and call refreshContentWidth() after changing the row set.
class ContentWidthListWidget final : public QListWidget
{
	Q_OBJECT
public:
	using QListWidget::QListWidget;

	[[nodiscard]] QSize sizeHint() const override        { return { contentWidth(), QListWidget::sizeHint().height() }; }
	[[nodiscard]] QSize minimumSizeHint() const override { return { contentWidth(), QListWidget::minimumSizeHint().height() }; }

	void refreshContentWidth()
	{
		updateGeometry();
		announceContentWidthChange();
	}

signals:
	// The hugging width changed: rows edited, or the vertical scrollbar appeared or went away.
	void contentWidthChanged(int width);

protected:
	void resizeEvent(QResizeEvent* event) override
	{
		QListWidget::resizeEvent(event);
		announceContentWidthChange();
	}

	// A scrollbar appearing resizes the viewport, not this widget, so resizeEvent() alone would miss its gutter.
	bool viewportEvent(QEvent* event) override
	{
		const bool handled = QListWidget::viewportEvent(event);
		if (event->type() == QEvent::Resize)
			announceContentWidthChange();
		return handled;
	}

private:
	[[nodiscard]] int contentWidth() const
	{
		int width = 2 * frameWidth() + qMax(sizeHintForColumn(0), 0);
		if (verticalScrollBar()->isVisible())
			width += verticalScrollBar()->sizeHint().width();
		return width;
	}

	void announceContentWidthChange()
	{
		const int width = contentWidth();
		if (width == _announcedWidth)
			return;

		_announcedWidth = width;
		// The parent layout caches sizeHint(): a stale one defeats a receiver that re-measures.
		updateGeometry();
		emit contentWidthChanged(width);
	}

private:
	int _announcedWidth = -1;
};
