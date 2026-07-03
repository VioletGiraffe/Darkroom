#pragma once

#include <QImage>
#include <QPointF>
#include <QStringList>
#include <QWidget>

#include <vector>

class QLabel;
class PhotoComparePane;

// N-way (2..4) photo comparison in a square grid of panes. All panes share ONE zoom+pan view, while each
// photo additionally carries its own alignment transform (uniform scale + offset) mapping it into the
// shared "subject" space - so photos of the same subject at different zoom/crop/resolution can be brought
// to the same apparent scale first, then compared under synchronized navigation. The default alignment
// normalizes by image height, which already compensates pure resolution differences (same shot exported
// at different sizes) with no user action.
//
// Controls (also summarized in the in-window hint bar):
//   wheel = synchronized zoom around the cursor; left-drag = synchronized pan; F / double-click = fit;
//   Ctrl+wheel / Ctrl+drag = adjust the hovered photo's alignment (scale / offset) alone;
//   A = two-point calibration: click the same two features in every photo - the photo that receives the
//       first point becomes the reference; the distance ratio gives each other photo's relative scale, the
//       midpoints its offset (right-click undoes a point, Esc cancels);
//   hold 1..N = flicker: every pane temporarily renders photo N under the shared view, release to revert;
//   Esc = close.
class PhotoCompareWindow final : public QWidget
{
public:
	// photoPaths: the photo files to compare (2..4 make sense; any count works). Paths that fail to load
	// are dropped with a warning.
	explicit PhotoCompareWindow(const QStringList& photoPaths, QWidget* parent = nullptr);
	~PhotoCompareWindow() override;

protected:
	void keyPressEvent(QKeyEvent* event) override;
	void keyReleaseEvent(QKeyEvent* event) override;

private:
	// The pane widgets are defined in the .cpp (no other consumer) and reach into the shared state below.
	friend class PhotoComparePane;

	struct Pane
	{
		QImage image;                 // full-res, EXIF-oriented
		std::vector<QImage> mipmaps;  // lazily built halving chain: [k] is image scaled by 2^-(k+1)
		double alignScale = 1.0;      // image -> subject space (subject space = the reference photo's pixel coords)
		QPointF alignOffset;          // image -> subject space
		QString caption;              // file name, drawn in the pane's corner
		std::vector<QPointF> calibPoints;  // image-space, at most 2; only populated while calibrating
	};

	// Coordinate mapping. widget = m_viewZoom * (alignScale * image + alignOffset) + m_viewPan; the pan is
	// in widget coordinates, shared across panes (they are equally sized grid cells, so the same transform
	// shows the same subject region in each).
	[[nodiscard]] QPointF subjectFromWidget(const QPointF& widgetPos) const;
	[[nodiscard]] QPointF widgetFromImage(const Pane& pane, const QPointF& imagePos) const;
	[[nodiscard]] QPointF imageFromWidget(const Pane& pane, const QPointF& widgetPos) const;

	// Returns the mip level to paint pane at effectiveScale (viewZoom * alignScale), building missing
	// levels on demand, and the residual scale the painter must still apply to that level.
	[[nodiscard]] const QImage& imageForScale(Pane& pane, double effectiveScale, double& residualScale);

	// Shared-view mutations (all panes follow).
	void zoomView(double factor, const QPointF& widgetAnchor);  // zoom keeping widgetAnchor fixed
	void panView(const QPointF& widgetDelta);
	void fitView();  // fit the reference photo's subject-space rect into a pane

	// Per-photo alignment mutations (Ctrl+wheel / Ctrl+drag).
	void adjustPaneScale(int index, double factor, const QPointF& widgetAnchor);
	void movePaneOffset(int index, const QPointF& widgetDelta);

	// Two-point calibration.
	[[nodiscard]] Qt::CursorShape idleCursor() const;  // cross while calibrating, open hand otherwise
	void setCalibrating(bool calibrating);
	void addCalibrationPoint(int index, const QPointF& imagePos);
	void undoCalibrationPoint(int index);
	void applyCalibration();  // called once every pane has 2 points

	void onPaneResized();
	void updateAllPanes();
	void updateHintText();

private:
	std::vector<Pane> m_panes;
	std::vector<PhotoComparePane*> m_paneWidgets;
	QLabel* m_hintLabel = nullptr;

	double m_viewZoom = 1.0;  // subject -> widget; 1.0 means the reference photo at 100% (1 image px = 1 widget px)
	QPointF m_viewPan;
	int m_flickerIndex = -1;   // >= 0: every pane renders this photo (a digit key is held)
	int m_refIndex = 0;        // the pane others calibrate against; re-chosen by each calibration session's first point
	bool m_calibrating = false;
	bool m_viewTouched = false;  // until the user navigates, pane resizes keep re-fitting the view
};
