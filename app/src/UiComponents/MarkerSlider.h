#pragma once

#include <QSlider>

// QSlider with two painted A/B markers; -1 means unset.
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
