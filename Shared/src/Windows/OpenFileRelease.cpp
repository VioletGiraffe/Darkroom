#include "Windows/OpenFileRelease.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/VideoPlayerWindow.h"
#include "Utils.h"

#include <algorithm>

void releaseMediaFilesHeldOpen(const QStringList& paths)
{
	QStringList pathKeys;
	for (const QString& path : paths)
	{
		if (!path.isEmpty())
			pathKeys.push_back(pathComparisonKey(path));
	}

	const auto isAtOrUnderAPath = [&pathKeys](const QString& filePath) {
		const QString fileKey = pathComparisonKey(filePath);
		return std::any_of(pathKeys.cbegin(), pathKeys.cend(), [&fileKey](const QString& key) {
			return fileKey == key || fileKey.startsWith(key + '/');
		});
	};
	VideoPlayerWindow::releaseFilesMatching(isAtOrUnderAPath);
	ImageViewerWindow::releaseFilesMatching(isAtOrUnderAPath);
}
