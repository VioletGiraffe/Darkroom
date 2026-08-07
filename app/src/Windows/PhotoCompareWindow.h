#pragma once

#include "threading/cworkerthread.h"

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <memory>
#include <stdint.h>
#include <vector>

class QCheckBox;
class QLabel;
class QSlider;
class QStackedLayout;
class PhotoComparePane;
class SegmentedToggle;
class Library;
struct AlignmentTransform;

// N-way aligned comparison. Pan/zoom is shared across panes; each photo has a similarity transform into the
// reference photo's pixel-coordinate "subject" space. Alignment may be automatic, two-point calibrated, or
// manually adjusted. The UI also provides flicker, absolute-difference, and full-view comparison modes.
class PhotoCompareWindow final : public QWidget
{
public:
	using PhotoRemovedHandler = std::function<void(const QString& filePath)>;

	// Empty paths open the drop-target state. Loading omits unreadable files and caps the set at 50.
	explicit PhotoCompareWindow(Library& library, const QStringList& photoPaths, QWidget* parent = nullptr,
		PhotoRemovedHandler photoRemovedHandler = {});
	~PhotoCompareWindow() override;

	// Opens a self-deleting window, or warns when fewer than two existing candidates remain.
	static void showForFiles(Library& library, const QStringList& candidatePaths, QWidget* parent,
		PhotoRemovedHandler photoRemovedHandler = {});

protected:
	void keyPressEvent(QKeyEvent* event) override;
	void keyReleaseEvent(QKeyEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	friend class PhotoComparePane;

	struct AlignmentMark
	{
		enum class Kind : uint8_t
		{
			Used,
			UsedCoarse,
			Outlier,
			Failed,
		};
		QPointF imagePos;
		Kind kind = Kind::Failed;
	};

	struct Photo
	{
		QImage image;
		QString filePath;
		std::vector<QImage> mipmaps;  // [k] is scaled by 2^-(k+1), built lazily
		double alignScale = 1.0;
		double alignRotation = 0.0;  // radians
		QPointF alignOffset;
		QString caption;
		std::vector<QPointF> calibPoints;
		std::vector<AlignmentMark> alignMarks;
		bool alignScored = false;
		bool alignSucceeded = false;
		double alignConfidence = 0.0;
		double alignBootstrapZncc = 0.0;
		double alignRotationSigma = 0.0;
		double alignTimeMs = 0.0;
	};

	// widget = viewZoom * (alignScale * R(alignRotation) * image + alignOffset) + viewPan.
	[[nodiscard]] QPointF subjectFromWidget(const QPointF& widgetPos) const;
	[[nodiscard]] QPointF widgetFromImage(const Photo& photo, const QPointF& imagePos) const;
	[[nodiscard]] QPointF imageFromWidget(const Photo& photo, const QPointF& widgetPos) const;

	// Chooses/builds a physical-scale mip and returns its remaining logical painter scale.
	[[nodiscard]] const QImage& imageForScale(Photo& photo, double effectiveScale, double devicePixelRatio, double& residualScale);

	struct PhotoLoadBatch;

	// One batch at a time; completion is applied on the GUI thread and drops are denied meanwhile.
	void addPhotosFromFiles(const QStringList& photoPaths);
	void applyLoadedPhotoBatch(PhotoLoadBatch& batch);
	void rebuildPaneGrid();
	void deletePhotoInteractive(int index);
	void removePhotoFromComparison(int index);

	[[nodiscard]] QRectF referenceSubjectRect() const;
	// Converts the align region to/from persisted reference-frame fractions.
	[[nodiscard]] QRectF normalizedFromSubjectRect(const QRectF& rect) const;
	[[nodiscard]] QRectF subjectRectFromNormalized(const QRectF& normalized) const;
	void setDefaultAlignment(Photo& photo);
	void resetToInitialState();

	void zoomView(double factor, const QPointF& widgetAnchor);
	void panView(const QPointF& widgetDelta);
	void fitView();
	void zoomToActualPixels();

	void adjustPhotoScale(int index, double factor, const QPointF& widgetAnchor);
	void movePhotoOffset(int index, const QPointF& widgetDelta);

	// Rebases subject space and AOI to reference pixels while keeping the reference fixed on screen.
	AlignmentTransform rebaseSubjectSpaceToReference();

	void autoAlignPhotos();

	[[nodiscard]] Qt::CursorShape idleCursor() const;
	void setCalibrating(bool calibrating);
	void addCalibrationPoint(int index, const QPointF& imagePos);
	void undoCalibrationPoint(int index);
	void applyCalibration();

	void setFullViewIndex(int index);
	void exitFullView();
	// Switches _viewStack to 'page', compensating the shared view for the viewport size change: a touched
	// view keeps its center subject point, an untouched one re-fits.
	void switchViewportPage(int page, const QSizeF& oldViewportSize, const QSizeF& newViewportSize);

	void setDifferenceMode(bool difference);

	void onPaneResized();
	void updateAllPanes();
	void updateHintText();

private:
	Library& _library;
	PhotoRemovedHandler _photoRemovedHandler;
	std::vector<Photo> _photos;
	std::vector<PhotoComparePane*> _paneWidgets;
	QWidget* _gridPage = nullptr;
	QLabel* _dropHintLabel = nullptr;
	PhotoComparePane* _fullPane = nullptr;
	QStackedLayout* _viewStack = nullptr;
	QSlider* _slider = nullptr;
	SegmentedToggle* _diffToggle = nullptr;
	QCheckBox* _ignoreRotationCheck = nullptr;
	QLabel* _hintLabel = nullptr;

	double _viewZoom = 1.0;
	QPointF _viewPan;
	QRectF _alignAoi;
	int _flickerIndex = -1;
	int _fullViewIndex = -1;
	// Edge of an alignment patch in subject units. The footprint is the same in subject space for both sides
	// of a pair (ref px directly; target px scaled by its alignment), so one number serves every mark.
	double _alignMarkSize = 0.0;
	int _refIndex = 0;
	bool _calibrating = false;
	bool _differenceMode = false;
	bool _showAlignDiagnostics = false;
	bool _viewTouched = false;

	std::shared_ptr<PhotoLoadBatch> _loadBatch;
	CWorkerThreadPool _workerPool;
};
