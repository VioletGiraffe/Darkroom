#pragma once

#include <QImage>
#include <QPointF>
#include <QStringList>
#include <QWidget>

#include <stdint.h>
#include <vector>

class QLabel;
class QSlider;
class QStackedLayout;
class PhotoComparePane;
class SegmentedToggle;

// N-way (2..4) photo comparison in a square grid of panes. All panes share ONE zoom+pan view, while each
// photo additionally carries its own alignment transform (a similarity: uniform scale + rotation + offset)
// mapping it into the shared "subject" space - so photos of the same subject at different zoom/crop/rotation/
// resolution can be brought to the same apparent scale first, then compared under synchronized navigation.
// The default alignment normalizes by image height, which already compensates pure resolution differences
// (same shot exported at different sizes) with no user action.
//
// Controls (also summarized in the in-window hint bar):
//   wheel = synchronized zoom around the cursor; left-drag = synchronized pan; F / double-click = fit;
//   Ctrl+wheel / Ctrl+drag = adjust the hovered photo's alignment (scale / offset) alone;
//   A = auto-align: estimates every photo's scale+rotation+offset against the reference (magic-alignment
//       library; rotation capture is small-angle - a few degrees); the patch evidence is drawn as
//       true-footprint squares (accent = used, orange = outlier, red = no match) until the next alignment;
//   Shift+A = two-point calibration: click the same two features in every photo - the photo that receives
//       the first point becomes the reference; the two point pairs define each other photo's similarity
//       exactly (scale from the distance ratio, rotation from the segment angles, offset from the
//       midpoints - so arbitrary angles work, beyond auto-align's range; right-click undoes a point,
//       Esc cancels);
//   hold 1..N = flicker: every pane temporarily renders photo N under the shared view, release to revert;
//   D / the Normal-Difference toggle = difference view: every photo except the reference renders as its
//       per-channel absolute difference against the reference, the reference stays normal;
//   bottom slider = full view: one pane covering the whole grid area, showing the photo at the slider's
//       position (drag to scrub between photos; Left/Right step it; held digit keys still override);
//   drop image files anywhere on the window = add them to the comparison;
//   right-click a pane = context menu: open the photo's containing folder, or make it the reference
//       (the reference pane is outlined in yellow);
//   Esc = leave full view / cancel calibration / close.
class PhotoCompareWindow final : public QWidget
{
public:
	// photoPaths: the photo files to compare (2..4 make sense; any count works; empty opens the window in
	// its drop-target state - the Tools menu route). Paths that fail to load are dropped with a warning.
	explicit PhotoCompareWindow(const QStringList& photoPaths, QWidget* parent = nullptr);
	~PhotoCompareWindow() override;

protected:
	void keyPressEvent(QKeyEvent* event) override;
	void keyReleaseEvent(QKeyEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	// The pane widgets are defined in the .cpp (no other consumer) and reach into the shared state below.
	friend class PhotoComparePane;

	struct AlignmentMark  // auto-align diagnostics: one patch's location in this photo
	{
		enum class Kind : uint8_t
		{
			Used,        // contributed to the fitted transform
			UsedCoarse,  // contributed, but only via a coarser pyramid level (unreliable at full res, e.g. defocused) - drawn dashed
			Outlier,     // matched well but disagrees with the fit (locally moved content, parallax, ...)
			Failed,      // no usable match (flat, weak, or outside the overlap)
		};
		QPointF imagePos;
		Kind kind = Kind::Failed;
	};

	struct Photo
	{
		QImage image;                 // full-res, EXIF-oriented
		QString filePath;             // full source path (the caption only shows the file name)
		std::vector<QImage> mipmaps;  // lazily built halving chain: [k] is image scaled by 2^-(k+1)
		double alignScale = 1.0;      // image -> subject space (subject space = the reference photo's pixel coords)
		double alignRotation = 0.0;   // radians; the R factor applied between the scale and the offset
		QPointF alignOffset;          // image -> subject space
		QString caption;              // file name, drawn in the pane's corner
		std::vector<QPointF> calibPoints;  // image-space, at most 2; only populated while calibrating
		std::vector<AlignmentMark> alignMarks;  // drawn as footprint squares; cleared when the next alignment starts
		bool alignScored = false;         // auto-align has evaluated this photo -> the two scores below are valid & shown
		double alignConfidence = 0.0;     // fitness of the last auto-align: mean patch ZNCC (AlignmentResult::confidence)
		double alignBootstrapZncc = 0.0;  // coarse whole-frame score of the last auto-align (AlignmentResult::bootstrapZncc)
		double alignRotationSigma = 0.0;  // radians: 1-sigma error bar of the fitted rotation (AlignmentResult::rotationSigma)
		double alignTimeMs = 0.0;         // runtime of the last alignImages call for this photo
	};

	// Coordinate mapping. widget = m_viewZoom * (alignScale * R(alignRotation) * image + alignOffset) +
	// m_viewPan; the pan is in widget coordinates, shared across panes (they are equally sized grid cells,
	// so the same transform shows the same subject region in each). The view itself has no rotation.
	[[nodiscard]] QPointF subjectFromWidget(const QPointF& widgetPos) const;
	[[nodiscard]] QPointF widgetFromImage(const Photo& photo, const QPointF& imagePos) const;
	[[nodiscard]] QPointF imageFromWidget(const Photo& photo, const QPointF& widgetPos) const;

	// Returns the mip level to paint the photo at effectiveScale (viewZoom * alignScale), building missing
	// levels on demand, and the residual scale the painter must still apply to that level.
	[[nodiscard]] const QImage& imageForScale(Photo& photo, double effectiveScale, double& residualScale);

	// Loads and appends photos (the constructor's initial batch, or files dropped onto the window), then
	// refreshes everything that depends on the photo count. Unreadable files are skipped with a warning.
	void addPhotosFromFiles(const QStringList& photoPaths);
	void rebuildPaneGrid();  // re-places every pane after a photo count change; shows the drop hint when empty

	// Shared-view mutations (all panes follow).
	void zoomView(double factor, const QPointF& widgetAnchor);  // zoom keeping widgetAnchor fixed
	void panView(const QPointF& widgetDelta);
	void fitView();  // fit the reference photo's subject-space rect into the current viewport (grid cell or full-view pane)

	// Per-photo alignment mutations (Ctrl+wheel / Ctrl+drag).
	void adjustPhotoScale(int index, double factor, const QPointF& widgetAnchor);
	void movePhotoOffset(int index, const QPointF& widgetDelta);

	// One-click automatic alignment of every photo against the reference (the A key).
	void autoAlignPhotos();

	// Two-point calibration.
	[[nodiscard]] Qt::CursorShape idleCursor() const;  // cross while calibrating, open hand otherwise
	void setCalibrating(bool calibrating);
	void addCalibrationPoint(int index, const QPointF& imagePos);
	void undoCalibrationPoint(int index);
	void applyCalibration();  // called once every pane has 2 points

	// Full (single-pane) view, driven by the bottom slider.
	void setFullViewIndex(int index);  // switches the full view to this photo, entering the mode if the grid is showing
	void exitFullView();

	void setDifferenceMode(bool difference);  // non-reference photos render as |photo - reference| (D key / bottom toggle)

	void onPaneResized();
	void updateAllPanes();
	void updateHintText();

private:
	std::vector<Photo> m_photos;
	std::vector<PhotoComparePane*> m_paneWidgets;  // the grid cells; the full-view pane is separate (m_fullPane)
	QWidget* m_gridPage = nullptr;             // page 0 of m_viewStack: the pane grid (or the drop hint when empty)
	QLabel* m_dropHintLabel = nullptr;         // centered "drop files" prompt, shown only while there are no photos
	PhotoComparePane* m_fullPane = nullptr;    // full-view page: one pane covering the whole grid area (index -1)
	QStackedLayout* m_viewStack = nullptr;     // page 0: the pane grid, page 1: m_fullPane
	QSlider* m_slider = nullptr;               // full-view photo picker; interacting with it enters the full view
	SegmentedToggle* m_diffToggle = nullptr;   // Normal / Difference, mirrors m_differenceMode
	QLabel* m_hintLabel = nullptr;

	double m_viewZoom = 1.0;  // subject -> widget; 1.0 means the reference photo at 100% (1 image px = 1 widget px)
	QPointF m_viewPan;
	int m_flickerIndex = -1;   // >= 0: every pane renders this photo (a digit key is held)
	int m_fullViewIndex = -1;  // >= 0: the full-view page is active, showing this photo (flicker keys still override)
	// Edge of an alignment patch in subject units. The footprint is the same in subject space for both sides
	// of a pair (ref px directly; target px scaled by its alignment), so one number serves every mark.
	double m_alignMarkSize = 0.0;
	int m_refIndex = 0;        // the pane others align against; re-chosen by each calibration session's first point or the pane context menu
	bool m_calibrating = false;
	bool m_differenceMode = false;
	bool m_viewTouched = false;  // until the user navigates, pane resizes keep re-fitting the view
};
