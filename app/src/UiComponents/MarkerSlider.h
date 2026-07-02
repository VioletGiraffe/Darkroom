#pragma once

#include <QSlider>

// A horizontal QSlider that additionally paints up to two position markers on top of the groove,
// used to denote the A/B endpoints of a playback loop. A marker value of -1 means "unset".
// Everything QSlider provides (mouse/keyboard seeking, styling, the seek signals) is kept as-is;
// this class only adds the marker rendering.
class MarkerSlider final : public QSlider
{
public:
	explicit MarkerSlider(Qt::Orientation orientation, QWidget* parent = nullptr);

	void setMarkerA(int value);
	void setMarkerB(int value);
	void clearMarkers();

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	int _markerA = -1;
	int _markerB = -1;
};
