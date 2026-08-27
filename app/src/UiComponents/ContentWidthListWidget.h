#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QListWidget>
#include <QScrollBar>
#include <QSize>
RESTORE_COMPILER_WARNINGS

// A QListWidget whose width hugs its widest delegated row plus frame and visible scrollbar. Callers bound
// pathological rows with setMaximumWidth() and call updateGeometry() after changing the row set.
class ContentWidthListWidget final : public QListWidget
{
public:
	using QListWidget::QListWidget;

	[[nodiscard]] QSize sizeHint() const override        { return { contentWidth(), QListWidget::sizeHint().height() }; }
	[[nodiscard]] QSize minimumSizeHint() const override { return { contentWidth(), QListWidget::minimumSizeHint().height() }; }

private:
	[[nodiscard]] int contentWidth() const
	{
		int width = 2 * frameWidth() + qMax(sizeHintForColumn(0), 0);
		if (verticalScrollBar()->isVisible())
			width += verticalScrollBar()->sizeHint().width();
		return width;
	}
};
