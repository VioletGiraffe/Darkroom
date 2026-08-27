#include "UiComponents/MarkerSlider.h"
#include "Theme/Theme.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QPainter>
#include <QStyleOptionSlider>
RESTORE_COMPILER_WARNINGS

#include <algorithm>

namespace {
// Qt reserves this much per tick side in QSlider::sizeHint; match it so a styled slider is no shorter than a native one.
constexpr int TickBand = 5;
constexpr int TickLength = 3;
}

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

QSize MarkerSlider::sizeHint() const
{
	QSize hint = QSlider::sizeHint();

	// QStyleSheetStyle can derive the whole size from the QSS boxes, dropping the tick allowance QSlider asked for.
	int bands = 0;
	if (tickPosition() & TicksAbove)
		++bands;
	if (tickPosition() & TicksBelow)
		++bands;
	if (bands > 0)
		hint.setHeight(std::max(hint.height(), Theme::current().metrics.sliderHandleDiameter + bands * TickBand));
	return hint;
}

void MarkerSlider::paintEvent(QPaintEvent* event)
{
	QSlider::paintEvent(event);

	const bool drawTicks = tickPosition() != NoTicks;
	if (!drawTicks && _markerA < 0 && _markerB < 0)
		return;

	// Match the style's exact handle-center calculation.
	QStyleOptionSlider opt;
	initStyleOption(&opt);
	const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
	const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
	const int available = groove.width() - handle.width();

	const auto xForValue = [&](int value) {
		return groove.left() + QStyle::sliderPositionFromValue(minimum(), maximum(), value, available, opt.upsideDown)
			+ handle.width() / 2;
	};

	QPainter painter{ this };

	// A zero tick interval means pageStep, as in QSlider's own tick drawing; zero from both would not terminate.
	const int interval = tickInterval() > 0 ? tickInterval() : pageStep();
	if (drawTicks && interval > 0)
	{
		painter.setPen(QPen{ Theme::current().palette.borderStrong, 1 });
		for (int value = minimum(); value <= maximum(); value += interval)
		{
			const int x = xForValue(value);
			if (tickPosition() & TicksAbove)
				painter.drawLine(x, handle.top() - 1 - TickLength, x, handle.top() - 1);
			if (tickPosition() & TicksBelow)
				painter.drawLine(x, handle.bottom() + 1, x, handle.bottom() + 1 + TickLength);
		}
	}

	if (_markerA >= 0 || _markerB >= 0)
	{
		painter.setPen(QPen{ Theme::current().starActive, 2 });
		for (const int value : { _markerA, _markerB })
		{
			if (value < 0)
				continue;

			const int x = xForValue(value);
			painter.drawLine(x, 0, x, height());
		}
	}
}
