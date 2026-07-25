#include "Core/CatalogIntegrity.h"
#include "Core/Catalog.h"
#include "Utils.h"

#include "utils/naturalsorting/cnaturalsorterqcollator.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

// Video verdicts are independent: a completed split needs real frames, every video needs a preview, and real
// frames behind a preview-only flag make that flag stale. Source absence overlays any combination. A photo has
// only its source file. The final disk walk reports frame folders and owned photos no entry claims.

namespace CatalogIntegrity {

IntegrityReport scan(const Catalog& catalog, const QString& rootFolder)
{
	IntegrityReport report;

	// Collect issues and the case-insensitive claimed paths needed by the untracked walk in one model pass.
	QSet<QString> knownFolders;
	QSet<QString> trackedSources;
	for (const auto& [id, entry] : catalog.mediaItems().asKeyValueRange())
	{
		knownFolders.insert(pathComparisonKey(entry.folder));
		if (!entry.sourcePath.isEmpty())
			trackedSources.insert(pathComparisonKey(entry.sourcePath));

		if (entry.type == Catalog::MediaType::Photo)
		{
			if (entry.sourcePath.isEmpty() || !QFileInfo::exists(entry.sourcePath))
				report.photoIssues.push_back({ id, entry.sourcePath, entry.referenced });
			continue;
		}

		MediaIssue issue;
		issue.id                = id;
		issue.folder            = entry.folder;
		issue.sourcePath        = entry.sourcePath;
		issue.sourcePresent     = !entry.sourcePath.isEmpty() && QFileInfo::exists(entry.sourcePath);
		issue.splitComplete     = entry.splitIntoFrames;
		issue.realFramesPresent = !listFrameImageFiles(QDir(entry.folder)).isEmpty();
		issue.previewPresent    = !listFrameImageFiles(QDir(Catalog::previewDirFor(entry.folder))).isEmpty();
		if (!issue.healthy())
			report.issues.push_back(issue);
	}

	// forEachFolder yields video frame folders, except under Photos where it yields label directories.
	forEachFolder(rootFolder, [&](const QString& storageFolder, const QString& folderPath) {
		if (storageFolder.compare(Catalog::PhotosDirectoryName.toString(), Qt::CaseInsensitive) == 0)
		{
			const QString labelName = QFileInfo(folderPath).fileName();
			// Owned photos include formats outside the extracted-frame set.
			QStringList photoFiles = collectFilesInDirectory(folderPath, /*recursive=*/false, isSupportedImageFile);
			std::ranges::sort(photoFiles, &NaturalSort::lessCaseSensitive);
			for (const QString& file : photoFiles)
				if (!trackedSources.contains(pathComparisonKey(file)))
					report.untrackedPhotos.push_back({ file, labelName });
			return;
		}
		if (knownFolders.contains(pathComparisonKey(folderPath)))
			return;
		if (listFrameImageFiles(QDir(folderPath)).isEmpty())
			return;
		report.untracked.push_back({ folderPath });
	});

	return report;
}

} // namespace CatalogIntegrity
