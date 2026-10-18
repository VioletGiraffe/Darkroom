#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

class QMouseEvent;

// Mutually exclusive text segments painted inside one rounded control.
class SegmentedToggle final : public QWidget
{
	Q_OBJECT
public:
	explicit SegmentedToggle(const QStringList& segments, QWidget* parent = nullptr);

	[[nodiscard]] int currentIndex() const { return _current; }

	// Programmatic selection is silent; only user clicks emit currentChanged.
	void setCurrentIndex(int index);

	[[nodiscard]] QSize sizeHint() const override;

signals:
	void currentChanged(int index);

protected:
	void paintEvent(QPaintEvent*) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void leaveEvent(QEvent* event) override;

private:
	[[nodiscard]] int segmentAt(const QPoint& pos) const;

	QStringList _segments;
	int _current = 0;
	int _hovered = -1;
};
