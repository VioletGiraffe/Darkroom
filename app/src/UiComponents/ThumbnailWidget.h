#pragma once

#include "UiComponents/DragGestureHelper.h"

#include <QImage>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <memory>

class QMimeData;

class ThumbnailWidget final : public QWidget {
public:
	struct LoadJob;

	ThumbnailWidget(const QString& filePath, const QString& caption, int thumbnailSize, QWidget* parent);
	// canvasSize is the card's image area: the frames are tiled across it (one slot each) and best-fit
	// into their slot. The widget grows by its caption strip and content margins around that area.
	// framed=false drops the border/hover/padding, leaving just the recessed matte image well — used by the
	// grid card, which draws its own frame (and hover) around the thumbnail + footer in MediaItemWidget.
	// filmStrip=true styles a video card as a film strip: a black base with the frames composited shorter so they
	// sit between reserved, sprocket-perforated top/bottom bands, and the inter-frame gaps showing as black lines.
	ThumbnailWidget(const QStringList& compositePaths, const QString& label, QWidget* parent, QSize canvasSize, bool dynamicSizeHint = true, bool framed = true, bool filmStrip = false);

	~ThumbnailWidget() override;

	[[nodiscard]] inline QString filePath() const { return _filePath; }

	// Re-renders the single displayed frame (path + caption) at the widget's current size. Used by
	// callers that swap the shown frame in place (e.g. CompareWindow stepping through frames).
	void loadFrame(const QString& path, const QString& caption);

	// Updates just the caption text and repaints - no re-render. Cheap enough to call on every
	// card when a grid re-sort renumbers labels, since the rendered image is unaffected.
	void setCaption(const QString& caption);

	// Invoked on Ctrl+wheel over the widget with the scroll direction (+1 zoom in, -1 zoom out). The widget
	// only detects the gesture and consumes the event; the handler owns the policy (what a step means,
	// bounds, persistence). A plain wheel is left to propagate so the surrounding view can scroll.
	void setOnMouseWheelCallback(std::function<void(int steps)> handler);

	QSize sizeHint() const override;

	// Height of the reserved top (and bottom) band on a film-strip card, for a given image-area height - the
	// space the frames give up to the perforated bands. Exposed so MediaItemWidget can lift its corner badges
	// clear of the bands. Scales with the card so the bands stay proportional across zoom.
	[[nodiscard]] static int filmStripBandHeight(int imageAreaHeight);

protected:
	void mouseDoubleClickEvent(QMouseEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;
	void paintEvent(QPaintEvent* /*event*/) override;

private:
	void applyStyleSettings();
	// Opens _filePath in the system's default viewer; warns and returns false if the handoff failed.
	bool openFile();

	// (Re)starts the async render of _sourcePaths into an _maxSize-sized canvas at the current DPR.
	void scheduleRender();
	// The logical size of the area the image is drawn into = contentsRect() minus the caption strip.
	[[nodiscard]] QSize currentImageArea() const;

	void disarmAsyncTask();

private:
	QString _filePath;
	QStringList _sourcePaths; // frame path(s) the loader renders into the canvas (1 = single frame, N = composite strip)
	QString _caption;
	QImage _image;
	QString _errorMessage; // non-empty after a completed load that produced no image (e.g. file not found)
	QSize _maxSize; // the image-area size the canvas is rendered to fill (deterministic, drives sizeHint)
	qreal _renderDpr = 0.0; // devicePixelRatioF() at the time of the most recently scheduled render; re-checked in paintEvent (see scheduleRender)

	std::shared_ptr<LoadJob> _job;  // shared control block for the in-flight two-stage load (read -> decode); see ThumbnailWidget.cpp
	bool _loadArmed = false;  // a dwell timer is pending to start the first render (see paintEvent); guards against stacking timers across repaints

	DragGestureHelper _dragHelper;
	std::function<QMimeData*()> _dragMimeDataFactory;

	std::function<void(int)> _onZoomRequested;

	const bool _bDynamicSizeHint = true;
	const bool _framed = true;  // false = borderless matte-only well (grid card draws its own frame)
	const bool _filmStrip = false;  // true = video film-strip styling (black base, reserved sprocket bands, black inter-frame gaps)
};
