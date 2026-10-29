#include "UiComponents/ThumbnailWidget.h"
#include "Theme/Theme.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "utils/painterutils.h"

DISABLE_COMPILER_WARNINGS
#include <QColor>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QUrl>
#include <QtMath>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <utility>
#include <variant>

static constexpr int THUMBNAIL_LABEL_HEIGHT = 20;
static constexpr int FILE_TYPE_ICON_SIDE = 16; // logical; the native size drawn is the largest that fits
static constexpr int FILE_TYPE_ICON_GAP = 3;

// QWidget does not derive contentsMargins from QSS border/padding; mirror the central sheet manually.
static constexpr int THUMBNAIL_BORDER_WIDTH = 1;
static constexpr int THUMBNAIL_PADDING = 2;
static constexpr int FRAMED_MARGIN = THUMBNAIL_BORDER_WIDTH + THUMBNAIL_PADDING;

// Avoid reading cards merely flicked past during scrolling.
static constexpr int LOAD_DWELL_MS = 100;

namespace {
	// The largest of the icon's native sizes within side logical pixels, unscaled: scaling blurs hand-tuned pixels.
	// Null when every native size is larger.
	QPixmap nativeIconPixmap(const QIcon& icon, int side, qreal dpr)
	{
		const int deviceSide = qFloor(side * dpr);
		QSize best;
		for (const QSize& size : icon.availableSizes())
		{
			if (size.width() <= deviceSide && size.height() <= deviceSide && size.width() > best.width())
				best = size;
		}
		if (best.isEmpty())
			return {};

		QPixmap pixmap = icon.pixmap(best, 1.0);
		pixmap.setDevicePixelRatio(dpr);
		return pixmap;
	}

	// Transparent composite gaps expose the black film base; sprockets occupy the reserved bands.
	void paintFilmBase(QPainter& painter, const QRect& imageArea)
	{
		painter.save();
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(18, 18, 18));
		const int matteRadius = Theme::current().metrics.thumbnailMatteRadius;
		painter.drawRoundedRect(imageArea, matteRadius, matteRadius);
		painter.restore();
	}

	void paintSprockets(QPainter& painter, const QRect& imageArea, int bandHeight)
	{
		const int holeHeight = qMax(3, bandHeight * 40 / 100);
		const int holeWidth  = qMax(4, holeHeight * 14 / 10);
		const int pitch      = holeWidth * 14 / 5;
		const int count      = qMax(2, imageArea.width() / pitch);
		const int cell       = imageArea.width() / count;
		const int radius     = qMax(1, holeHeight / 3);
		const int topY       = imageArea.top() + (bandHeight - holeHeight) / 2;
		const int bottomY    = imageArea.bottom() - bandHeight + (bandHeight - holeHeight) / 2;

		painter.save();
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(233, 231, 225));   // film base showing through the perforations - warm off-white, fixed in both themes
		for (int i = 0; i < count; ++i)
		{
			const int x = imageArea.left() + i * cell + (cell - holeWidth) / 2;
			painter.drawRoundedRect(QRect(x, topY, holeWidth, holeHeight), radius, radius);
			painter.drawRoundedRect(QRect(x, bottomY, holeWidth, holeHeight), radius, radius);
		}
		painter.restore();
	}

} // anonymous namespace

ThumbnailWidget::ThumbnailWidget(Source source, const QString& caption, const Options& options, QWidget* parent)
	: QWidget(parent), _source{ std::move(source) }, _options{ options }, _caption{ caption }
{
	applyStyleSettings();
}

void ThumbnailWidget::setSource(Source source)
{
	_request = {};
	_source = std::move(source);
	_image = {};
	_errorMessage.clear();
	_nothingToShow = false;
	_fileTypeIcon = {};
	_fileTypePixmap = {};
	_renderedBox = {};
	_renderedDpr = 0.0;
	updateGeometry();
	update();
}

void ThumbnailWidget::setCaption(const QString& caption)
{
	if (_caption == caption)
		return;

	_caption = caption;
	update();
}

void ThumbnailWidget::setFileTypeIconLookup(FileTypeIconLookup lookup)
{
	_fileTypeIconLookup = lookup;
}

QSize ThumbnailWidget::currentImageArea() const
{
	const QRect c = contentsRect();
	const int labelHeight = _caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;
	return QSize(c.width(), c.height() - labelHeight);
}

QSize ThumbnailWidget::imageAreaBounds() const
{
	return _options.canvasSize.isEmpty() ? currentImageArea() : _options.canvasSize;
}

QSize ThumbnailWidget::renderBox() const
{
	const QSize bounds = imageAreaBounds();
	// Film frames render between the bands while the bounds retain the full card footprint.
	if (_options.filmStrip)
		return QSize(bounds.width(), qMax(1, bounds.height() - 2 * filmStripBandHeight(bounds.height())));
	return bounds;
}

int ThumbnailWidget::filmStripBandHeight(int imageAreaHeight)
{
	// Scale with zoom but cap the bands at both extremes.
	return qBound(7, imageAreaHeight / 11, 18);
}

bool ThumbnailWidget::isRenderedFor(QSize box, qreal dpr) const
{
	return box == _renderedBox && qFuzzyCompare(dpr, _renderedDpr);
}

void ThumbnailWidget::renderIfStale()
{
	const QSize box = renderBox();
	const qreal dpr = devicePixelRatioF();
	if (_loadArmed || box.isEmpty() || isRenderedFor(box, dpr))
		return;

	if (std::optional<ImageSource::Result> result = std::visit([&](const auto& source) { return source.renderNow(box, dpr); }, _source))
	{
		_request = {};
		_renderedBox = box;
		_renderedDpr = dpr;
		applyResult(std::move(*result));
		updateGeometry();
		return;
	}

	_loadArmed = true;
	QTimer::singleShot(LOAD_DWELL_MS, this, [this] {
		_loadArmed = false;
		if (!visibleRegion().isEmpty())
			startAsyncRender();
	});
}

void ThumbnailWidget::startAsyncRender()
{
	const QSize box = renderBox();
	const qreal dpr = devicePixelRatioF();
	if (box.isEmpty() || isRenderedFor(box, dpr))
		return;

	_renderedBox = box;
	_renderedDpr = dpr;
	auto done = [this](ImageSource::Result result) {
		applyResult(std::move(result));
		updateGeometry();
		update();
	};
	_request = std::visit([&](const auto& source) { return source.renderAsync(box, dpr, this, std::move(done)); }, _source);
}

void ThumbnailWidget::applyResult(ImageSource::Result result)
{
	_nothingToShow = result.image.isNull() && result.error.isEmpty();
	_image = std::move(result.image);
	_errorMessage = result.error.isEmpty() ? QString{} : tr("Failed to load:") + '\n' + result.error;
	_fileTypeIcon = _fileTypeIconLookup ? _fileTypeIconLookup(result.fileType) : QIcon{};
	_fileTypePixmap = {};
}

void ThumbnailWidget::setOnActivatedCallback(std::function<void()> handler)
{
	_onActivated = std::move(handler);
}

void ThumbnailWidget::setDragFilePath(const QString& path)
{
	if (path.isEmpty())
	{
		_dragMimeDataFactory = {};
		return;
	}

	_dragMimeDataFactory = [path] {
		auto* mime = new QMimeData();
		mime->setUrls({ QUrl::fromLocalFile(path) });
		return mime;
	};
}

QSize ThumbnailWidget::sizeHint() const
{
	if (!_options.dynamicSizeHint)
		return _options.canvasSize.isEmpty() ? QWidget::sizeHint() : _options.canvasSize;

	// Dynamic consumers tighten after load; QListWidget callers must update their cached item hint themselves.
	const QSize imageArea = _image.isNull() ? imageAreaBounds() : _image.deviceIndependentSize().toSize();
	return imageArea + chromeSize(_options, !_caption.isEmpty());
}

QSize ThumbnailWidget::chromeSize(const Options& options, bool captioned)
{
	const int margins = options.framed ? 2 * FRAMED_MARGIN : 0;
	return { margins, margins + (captioned ? THUMBNAIL_LABEL_HEIGHT : 0) };
}

void ThumbnailWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton && _onActivated)
		_onActivated();

	QWidget::mouseDoubleClickEvent(event);
}

void ThumbnailWidget::mousePressEvent(QMouseEvent* event)
{
	_dragHelper.mousePressed(event);
	QWidget::mousePressEvent(event);
}

void ThumbnailWidget::mouseMoveEvent(QMouseEvent* event)
{
	if (_dragMimeDataFactory)
	{
		if (_dragHelper.tryStartDrag(this, event, _dragMimeDataFactory, Qt::CopyAction))
			return;
	}
	QWidget::mouseMoveEvent(event);
}

void ThumbnailWidget::paintEvent(QPaintEvent*)
{
	// Rendering waits for paint: only shown widgets load, and a parentless card's pixel ratio is stale until shown.
	renderIfStale();

	QPainter painter(this);

	const QRect content = contentsRect();
	const int labelHeight = _caption.isEmpty() ? 0 : THUMBNAIL_LABEL_HEIGHT;
	const QRect imageArea(content.left(), content.top(), content.width(), content.height() - labelHeight);

	const int bandHeight = _options.filmStrip ? filmStripBandHeight(imageArea.height()) : 0;
	if (_options.filmStrip)
		paintFilmBase(painter, imageArea);

	if (!_image.isNull())
	{
		// Blit 1:1 so sizing mistakes remain visible; only transient oversize during rerender scales down.
		const QSizeF imageLogical = _image.deviceIndependentSize();
		const bool fits = imageLogical.width() <= imageArea.width() && imageLogical.height() <= imageArea.height();
		QRectF r(QPointF(0, 0), fits ? imageLogical : imageLogical.scaled(imageArea.size(), Qt::KeepAspectRatio));
		r.moveCenter(QRectF(imageArea).center());
		if (fits)
			r.moveTopLeft(snappedToDevicePixel(painter, r.topLeft()));

		const auto blit = [&] {
			if (fits)
				painter.drawImage(r.topLeft(), _image);
			else
			{
				painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
				painter.drawImage(r, _image);
			}
		};

		// Film-strip bands keep square frame corners inside the rounded film base.
		if (_options.filmStrip || !std::visit([](const auto& source) { return source.roundsCorners(); }, _source))
			blit();
		else
		{
			// QSS border-radius does not clip custom painting, so clip the image explicitly to the matte.
			painter.save();
			QPainterPath roundedImage;
			const int matteRadius = Theme::current().metrics.thumbnailMatteRadius;
			roundedImage.addRoundedRect(r, matteRadius, matteRadius);
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setClipPath(roundedImage);
			blit();
			painter.restore();
		}
	}
	else if (!_errorMessage.isEmpty())
		painter.drawText(content, Qt::AlignCenter | Qt::TextWordWrap, _errorMessage);
	else if (_nothingToShow)
		painter.drawText(content, Qt::AlignCenter, tr("No preview"));
	else
		painter.drawText(content, Qt::AlignCenter, tr("Loading..."));

	if (_options.filmStrip)
		paintSprockets(painter, imageArea, bandHeight);

	if (!_caption.isEmpty())
	{
		QRect labelRect(content.left() + 2, content.bottom() - THUMBNAIL_LABEL_HEIGHT + 1, content.width() - 4, THUMBNAIL_LABEL_HEIGHT);
		if (!_fileTypeIcon.isNull())
		{
			const qreal dpr = devicePixelRatioF();
			if (_fileTypePixmap.isNull() || !qFuzzyCompare(_fileTypePixmap.devicePixelRatio(), dpr))
				_fileTypePixmap = nativeIconPixmap(_fileTypeIcon, FILE_TYPE_ICON_SIDE, dpr);
			if (!_fileTypePixmap.isNull())
			{
				const QSizeF iconLogical = _fileTypePixmap.deviceIndependentSize();
				const QPointF iconPos = snappedToDevicePixel(painter,
					QPointF(labelRect.right() + 1 - iconLogical.width(), labelRect.top() + (labelRect.height() - iconLogical.height()) / 2));
				painter.drawPixmap(iconPos, _fileTypePixmap);
				labelRect.setRight(qFloor(iconPos.x()) - FILE_TYPE_ICON_GAP);
			}
		}
		painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignHCenter, painter.fontMetrics().elidedText(_caption, Qt::ElideMiddle, labelRect.width()));
	}
}

void ThumbnailWidget::applyStyleSettings()
{
	setAttribute(Qt::WA_StyledBackground);

	// Object-name rules in the central sheet avoid per-instance polish across hundreds of thumbnails.
	if (!_options.framed)
	{
		setObjectName("cardThumbnailWell");
		setContentsMargins(0, 0, 0, 0);
		return;
	}

	setObjectName("framedThumbnail");
	setAttribute(Qt::WA_Hover);
	setContentsMargins(FRAMED_MARGIN, FRAMED_MARGIN, FRAMED_MARGIN, FRAMED_MARGIN);
}
