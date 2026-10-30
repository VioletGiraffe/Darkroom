#pragma once

#include "Core/AsyncDelivery.h"
#include "ImageDecoding/ImageDecoding.h"
#include "compiler/compiler_warnings_control.h"
#include "widgets/cimageviewerwidget.h"

DISABLE_COMPILER_WARNINGS
#include <QSize>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <functional>

class QObject;

// Loads one image for ImageViewerWindow: reads on IoThreadPool, decodes on cpuThreadPool(), delivers on the context's thread.
namespace ImageViewerLoading {

struct Stage {
	enum class Kind {
		Preview,  // file: a reduced rendition of the image; finalSize: the final image's upright size
		Final,    // file: the image
		Animated, // the file is left to CImageViewerWidget::displayImage, which plays it
		Failed,   // error: why
	};

	Kind kind = Kind::Failed;
	CImageViewerWidget::DecodedFile file;
	QSize finalSize;
	QString error;
};

// Previews come first where the final image takes longer: a JPEG's DCT-reduced decode that still covers uprightBox, and a
// camera raw's embedded preview while LibRaw renders the raw. Then exactly one of Final, Animated or Failed.
[[nodiscard]] AsyncRequest start(const QString& path, ImageDecoding::RawRendering rawRendering, QSize uprightBox, QObject* context,
	std::function<void(Stage)> done);

} // namespace ImageViewerLoading
