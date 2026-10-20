#pragma once

#include "UiComponents/ImageSource.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QIcon>
#include <QSize>
RESTORE_COMPILER_WARNINGS

#include <optional>

// A file icon at a fraction of the box, for entries without an image preview (folders, videos).
class IconSource final {
public:
	// fileType: the type of the file the icon stands for, reported in every result.
	explicit IconSource(QIcon icon, QByteArray fileType = {});

	[[nodiscard]] std::optional<ImageSource::Result> renderNow(QSize box, qreal dpr) const;
	[[nodiscard]] ImageSource::Request renderAsync(QSize box, qreal dpr, QObject* context, std::function<void(ImageSource::Result)> done) const;
	[[nodiscard]] bool roundsCorners() const { return false; }

private:
	QIcon _icon;
	QByteArray _fileType;
};
