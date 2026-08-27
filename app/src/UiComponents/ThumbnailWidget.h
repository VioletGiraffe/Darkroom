#pragma once

#include "UiComponents/DragGestureHelper.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QImage>
#include <QStringList>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <memory>

class QMimeData;

class ThumbnailWidget final : public QWidget {
public:
	struct LoadJob;

	ThumbnailWidget(const QString& filePath, const QString& caption, int thumbnailSize, QWidget* parent);
	// Tiles one best-fit frame per slot in canvasSize. Empty paths render a final "No preview" state.
	// framed=false leaves only the matte; filmStrip reserves perforated bands around a black frame strip.
	ThumbnailWidget(const QStringList& compositePaths, const QString& label, QWidget* parent, QSize canvasSize, bool dynamicSizeHint = true,
		bool framed = true, bool filmStrip = false);

	~ThumbnailWidget() override;

	[[nodiscard]] inline QString filePath() const { return _filePath; }

	void loadFrame(const QString& path, const QString& caption);

	// Repaints without re-rendering the image.
	void setCaption(const QString& caption);

	// Ctrl+wheel emits signed steps and is consumed; plain wheel propagates to the surrounding view.
	void setOnMouseWheelCallback(std::function<void(int steps)> handler);

	// Called on a double-click, and only for a thumbnail that has a file path. Without a handler a double-click does nothing.
	void setOnActivatedCallback(std::function<void()> handler);

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

	void scheduleRender();
	[[nodiscard]] QSize currentImageArea() const;

	void disarmAsyncTask();

private:
	QString _filePath;
	QStringList _sourcePaths;
	QString _caption;
	QImage _image;
	QString _errorMessage;
	QSize _maxSize;
	qreal _renderDpr = 0.0;

	std::shared_ptr<LoadJob> _job;
	bool _loadArmed = false;

	DragGestureHelper _dragHelper;
	std::function<QMimeData*()> _dragMimeDataFactory;

	std::function<void(int)> _onZoomRequested;
	std::function<void()> _onActivated;

	const bool _bDynamicSizeHint = true;
	const bool _framed = true;
	const bool _filmStrip = false;
};
