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
		Progress, // progress: the fraction of LibRaw's rendering of a camera raw done, 0 to 1
		Final,    // file: the image
		Animated, // the file is left to CImageViewerWidget::displayImage, which plays it
		Failed,   // error: why
	};

	Kind kind = Kind::Failed;
	CImageViewerWidget::DecodedFile file;
	QSize finalSize;
	double progress = 0.0;
	QString error;
};

// A preview decodes in parallel with the final image where that takes longer:
//   a JPEG whose full decode this machine is predicted to take 100 ms or more - at 1/8;
//   a camera raw's embedded JPEG while LibRaw renders the raw - fitted to uprightBox where that is 1/2 or less, else whole.
// Progress comes, in any order with a preview, while LibRaw renders.
// Exactly one of Final, Animated or Failed ends the load; a preview may still arrive after it.
[[nodiscard]] AsyncRequest start(const QString& path, ImageDecoding::RawRendering rawRendering, QSize uprightBox, QObject* context,
	std::function<void(Stage)> done);

} // namespace ImageViewerLoading
