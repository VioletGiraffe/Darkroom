#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QImage>
#include <QString>
RESTORE_COMPILER_WARNINGS

// Types shared by the image sources a ThumbnailWidget shows. Every source has these, called on the GUI thread:
//   std::optional<Result> renderNow(QSize box, qreal dpr) const - the result when it is cheap to produce there; empty otherwise
//   AsyncRequest renderAsync(QSize box, qreal dpr, QObject* context, std::function<void(Result)> done) const - only called
//     after renderNow returned empty; done runs on context's thread, never after context is destroyed
//   bool roundsCorners() const - whether the shown image's corners are rounded to the matte
// An image fits the requested box: logical size within it, device pixel ratio as requested.
namespace ImageSource {

struct Result {
	QImage image;        // null with an empty error: nothing to show
	QString error;       // non-empty: why there is no image
	QByteArray fileType; // ImageDecoding's format for decoded images, given by the caller otherwise; empty when unknown
};

} // namespace ImageSource
