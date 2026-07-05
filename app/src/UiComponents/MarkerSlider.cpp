#include "UiComponents/MarkerSlider.h"
#include "Theme/Theme.h"

#include <QPainter>
#include <QStyleOptionSlider>

MarkerSlider::MarkerSlider(Qt::Orientation orientation, QWidget* parent) : QSlider(orientation, parent) {}

void MarkerSlider::setMarkerA(int value)
{
	_markerA = value;
	update();
}

void MarkerSlider::setMarkerB(int value)
{
	_markerB = value;
	update();
}

void MarkerSlider::clearMarkers()
{
	_markerA = _markerB = -1;
	update();
}

void MarkerSlider::paintEvent(QPaintEvent* event)
{
	QSlider::paintEvent(event);

	if (_markerA < 0 && _markerB < 0)
		return;

	// Derive the exact handle-center x for a value the same way the style positions the handle,
	// so the markers line up with where the handle sits at those values rather than approximating.
	QStyleOptionSlider opt;
	initStyleOption(&opt);
	const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
	const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
	const int available = groove.width() - handle.width();

	QPainter painter{ this };
	painter.setPen(QPen{ QColor{ QString::fromLatin1(Theme::StarActive) }, 2 }); // gold: legible on both themes, and a strong hue contrast with the accent-filled groove

	for (const int value : { _markerA, _markerB })
	{
		if (value < 0)
			continue;

		const int pos = QStyle::sliderPositionFromValue(minimum(), maximum(), value, available, opt.upsideDown);
		const int x = groove.left() + pos + handle.width() / 2;
		painter.drawLine(x, 0, x, height());
	}
}
