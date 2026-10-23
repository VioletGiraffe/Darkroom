#include "Core/CatalogIntegrity.h"
#include "Core/Catalog.h"
#include "Utils.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFileInfo>
#include <QSet>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>

// Video verdicts are independent: a completed split needs real frames, every video needs a preview, and real
// frames behind a preview-only flag make that flag stale. Source absence overlays any combination. A photo has
// only its source file. Every item is also checked for label ids absent from the registry. The final disk walk
// reports frame folders and owned photos no entry claims.

namespace CatalogIntegrity {

IntegrityReport scan(const Catalog& catalog, const QString& rootFolder, const ScanProgress& progress)
{
	IntegrityReport report;

	QSet<LabelId> knownLabelIds;
	for (const Catalog::Label& label : catalog.allLabels())
		knownLabelIds.insert(label.id);

	// Collect issues and the case-insensitive claimed paths needed by the untracked walk in one model pass.
	QSet<QString> knownFolders;
	QSet<QString> trackedSources;
	const int itemCount = catalog.mediaItemCount();
	int checkedItems = 0;
	for (const auto& [id, entry] : catalog.mediaItems().asKeyValueRange())
	{
		if (progress)
			progress(ScanPhase::Items, checkedItems++, itemCount);

		knownFolders.insert(pathComparisonKey(entry.folder));
		if (!entry.sourcePath.isEmpty())
			trackedSources.insert(pathComparisonKey(entry.sourcePath));

		DanglingLabelIssue labelIssue{ .id = id };
		for (const LabelId labelId : entry.labelIds)
			if (!knownLabelIds.contains(labelId))
				labelIssue.missingLabelIds.push_back(labelId);
		if (!labelIssue.missingLabelIds.empty())
			report.danglingLabelIssues.push_back(std::move(labelIssue));

		if (entry.type == Catalog::MediaType::Photo)
		{
			if (entry.sourcePath.isEmpty() || !QFileInfo::exists(entry.sourcePath))
				report.photoIssues.push_back({ id, entry.sourcePath, entry.referenced });
			continue;
		}

		MediaIssue issue = inspectVideo(catalog, id);
		if (!issue.healthy())
			report.issues.push_back(std::move(issue));
	}

	// forEachFolder yields video frame folders, except under Photos where it yields label directories.
	int walkedFolders = 0;
	forEachFolder(rootFolder, [&](const QString& storageFolder, const QString& folderPath) {
		if (progress)
			progress(ScanPhase::Folders, walkedFolders++, 0);

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

MediaIssue inspectVideo(const Catalog& catalog, const MediaId& id)
{
	const auto it = catalog.mediaItems().constFind(id);
	assert_and_return_r(it != catalog.mediaItems().cend() && it->type == Catalog::MediaType::Video, {});

	MediaIssue issue;
	issue.id                = id;
	issue.folder            = it->folder;
	issue.sourcePath        = it->sourcePath;
	issue.sourcePresent     = !it->sourcePath.isEmpty() && QFileInfo::exists(it->sourcePath);
	issue.splitComplete     = it->splitIntoFrames;
	issue.realFramesPresent = !listFrameImageFiles(QDir(it->folder)).isEmpty();
	issue.previewPresent    = !listFrameImageFiles(QDir(Catalog::previewDirFor(it->folder))).isEmpty();
	return issue;
}

} // namespace CatalogIntegrity
