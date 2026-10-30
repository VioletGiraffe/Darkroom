#pragma once

#include "Core/AsyncDelivery.h"
#include "UiComponents/ImageSource.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QSize>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <optional>

// Image files decoded and tiled side by side, each best-fit to its slot of the box. No paths: nothing to show.
// The result's file type is the format of the first file recognised, decodable or not.
class ImageFilesSource final {
public:
	explicit ImageFilesSource(QStringList paths);

	[[nodiscard]] std::optional<ImageSource::Result> renderNow(QSize box, qreal dpr) const;
	[[nodiscard]] AsyncRequest renderAsync(QSize box, qreal dpr, QObject* context, std::function<void(ImageSource::Result)> done) const;
	[[nodiscard]] bool roundsCorners() const { return true; }

private:
	QStringList _paths;
};
