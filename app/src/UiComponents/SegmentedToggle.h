#pragma once

#include <QStringList>
#include <QWidget>

class QMouseEvent;

// A small segmented control: a row of mutually-exclusive segments inside one rounded pill, the selected
// one filled. A non-stock stand-in for a pair of radio buttons or a two-state toggle button. Generic over
// its segment labels so it serves both the sidebar's OR/AND combine mode and the sort popover's
// field/direction toggles.
class SegmentedToggle final : public QWidget
{
	Q_OBJECT
public:
	explicit SegmentedToggle(const QStringList& segments, QWidget* parent = nullptr);

	[[nodiscard]] int currentIndex() const { return _current; }

	// Selects a segment programmatically. Silent by design (no currentChanged) so callers can restore
	// persisted state without re-triggering the work the signal drives; only user clicks emit.
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
