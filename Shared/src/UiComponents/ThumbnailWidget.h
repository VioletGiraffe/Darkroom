#pragma once

#include "UiComponents/DragGestureHelper.h"
#include "UiComponents/IconSource.h"
#include "UiComponents/ImageFilesSource.h"
#include "UiComponents/ImageSource.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QImage>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <variant>

class QMimeData;

// Shows a source's image in a matte, with an optional frame, caption and film-strip treatment.
// Renders on paint, so only shown widgets load; a size or pixel-ratio change re-renders, keeping the old image until then.
class ThumbnailWidget final : public QWidget {
public:
	// Each alternative provides the members listed in ImageSource.h.
	using Source = std::variant<ImageFilesSource, IconSource>;

	struct Options {
		QSize canvasSize;            // the image area; empty: follows the widget's size
		bool dynamicSizeHint = true; // the size hint tightens to the loaded image
		bool framed = true;          // false leaves only the matte
		bool filmStrip = false;      // perforated bands around a black frame strip
	};

	ThumbnailWidget(Source source, const QString& caption, const Options& options, QWidget* parent);

	// Clears the shown image; the new one renders on the next paint.
	void setSource(Source source);

	void setCaption(const QString& caption);

	// Ctrl+wheel emits signed steps and is consumed; plain wheel propagates to the surrounding view.
	void setOnMouseWheelCallback(std::function<void(int steps)> handler);

	// Called on a double-click. Without a handler a double-click does nothing.
	void setOnActivatedCallback(std::function<void()> handler);

	// Drags the file as a URL; empty: no drag, so the surrounding view can drive one instead.
	void setDragFilePath(const QString& path);

	QSize sizeHint() const override;

	// Exposed so MediaItemWidget can keep corner badges clear of proportional film bands.
	[[nodiscard]] static int filmStripBandHeight(int imageAreaHeight);

protected:
	void mouseDoubleClickEvent(QMouseEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;
	void paintEvent(QPaintEvent* /*event*/) override;

private:
	void applyStyleSettings();

	[[nodiscard]] QSize currentImageArea() const;
	[[nodiscard]] QSize imageAreaBounds() const;
	[[nodiscard]] QSize renderBox() const;

	// Whether the shown or pending image was requested for this box and ratio.
	[[nodiscard]] bool isRenderedFor(QSize box, qreal dpr) const;
	// Renders at once when the source can, else after the widget stays visible for a dwell.
	void renderIfStale();
	void startAsyncRender();
	void applyResult(ImageSource::Result result);

private:
	Source _source;
	const Options _options;
	QString _caption;

	QImage _image;
	QString _errorMessage;
	bool _nothingToShow = false;

	QSize _renderedBox; // empty: nothing requested yet
	qreal _renderedDpr = 0.0;
	ImageSource::Request _request;
	bool _loadArmed = false;

	DragGestureHelper _dragHelper;
	std::function<QMimeData*()> _dragMimeDataFactory;

	std::function<void(int)> _onZoomRequested;
	std::function<void()> _onActivated;
};
