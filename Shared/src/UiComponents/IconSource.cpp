#include "UiComponents/IconSource.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "hash/wheathash.hpp"

DISABLE_COMPILER_WARNINGS
#include "resize/qimage_resize.h"

#include <QImage>
#include <QPixmap>
#include <QPixmapCache>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>

// Sized as QIcon::paint sizes it, but downscaled smoothly: native icons are often much larger than the tile (256 px on Windows).
// Keyed by pixel content: QFileIconProvider returns a distinct QIcon per file, even for identical icons.
static QPixmap scaledIconPixmap(const QIcon& icon, const QSize& logicalBox, qreal dpr)
{
	const QImage native = icon.pixmap(logicalBox, dpr).toImage();

	QSize logicalSize = native.size();
	if (logicalSize.width() > logicalBox.width() || logicalSize.height() > logicalBox.height())
		logicalSize.scale(logicalBox, Qt::KeepAspectRatio);
	const QSize targetSize = logicalSize * dpr;

	if (targetSize == native.size())
	{
		QPixmap result = QPixmap::fromImage(native);
		result.setDevicePixelRatio(dpr);
		return result;
	}

	const uint64_t contentHash = wheathash64(native.constBits(), static_cast<uint64_t>(native.sizeInBytes()));
	const QString key = QStringLiteral("IconTile_%1_%2_%3_%4x%5@%6")
		.arg(contentHash).arg(static_cast<int>(native.format())).arg(native.width()).arg(targetSize.width()).arg(targetSize.height()).arg(dpr);
	QPixmap result;
	if (QPixmapCache::find(key, &result))
		return result;

	QImage scaled{ targetSize, native.format() };
	if (!ImageProcessing::resize(scaled, native))
		scaled = native.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	scaled.setDevicePixelRatio(dpr);
	result = QPixmap::fromImage(std::move(scaled));
	QPixmapCache::insert(key, result);
	return result;
}

IconSource::IconSource(QIcon icon)
	: _icon{ std::move(icon) }
{
}

std::optional<ImageSource::Result> IconSource::renderNow(QSize box, qreal dpr) const
{
	const int iconSide = std::min(box.width(), box.height()) * 3 / 5;
	return ImageSource::Result{ .image = scaledIconPixmap(_icon, QSize(iconSide, iconSide), dpr).toImage() };
}

ImageSource::Request IconSource::renderAsync(QSize /*box*/, qreal /*dpr*/, QObject* /*context*/, std::function<void(ImageSource::Result)> /*done*/) const
{
	assert_unconditional_r("renderNow always answers");
	return {};
}
