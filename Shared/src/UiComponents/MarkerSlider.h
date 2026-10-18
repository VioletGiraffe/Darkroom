#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QSlider>
RESTORE_COMPILER_WARNINGS

// QSlider that paints the marks QStyleSheetStyle leaves out: tick marks at tickInterval(), plus two optional A/B
// markers (-1 means unset). Horizontal orientation only.
class MarkerSlider final : public QSlider
{
public:
	explicit MarkerSlider(Qt::Orientation orientation, QWidget* parent = nullptr);

	void setMarkerA(int value);
	void setMarkerB(int value);
	void clearMarkers();

	[[nodiscard]] QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	int _markerA = -1;
	int _markerB = -1;
};
