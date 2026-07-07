#pragma once

#include <QListWidget>
#include <QScrollBar>
#include <QSize>

// A QListWidget that reports the width its widest row actually needs - the widest item's delegate sizeHint,
// plus the frame and, when shown, the vertical scrollbar - rather than QListView's large fixed default, so a
// containing layout or splitter can hug the list to its content. Height keeps the base QListWidget behavior;
// per-row padding comes from whatever delegate paints the rows (the default delegate's own item padding, or a
// custom delegate that folds breathing room into its sizeHint).
//
// Growth is bounded by the caller on the instance: setMaximumWidth() caps it so one pathologically long row
// can't blow the panel up (the row elides at the cap). Leaving the minimum width unset lets the minimum track
// the content up to that cap; an explicit setMinimumWidth() instead pins a fixed floor. Call updateGeometry()
// when the row set changes so the layout re-fits.
class ContentWidthListWidget final : public QListWidget
{
public:
	using QListWidget::QListWidget;

	[[nodiscard]] QSize sizeHint() const override        { return { contentWidth(), QListWidget::sizeHint().height() }; }
	[[nodiscard]] QSize minimumSizeHint() const override { return { contentWidth(), QListWidget::minimumSizeHint().height() }; }

private:
	[[nodiscard]] int contentWidth() const
	{
		int width = 2 * frameWidth() + qMax(sizeHintForColumn(0), 0);   // widest row via the delegate; -1 when empty
		if (verticalScrollBar()->isVisible())
			width += verticalScrollBar()->sizeHint().width();
		return width;
	}
};
