#include "Core/CatalogIntegrity.h"
#include "Core/Catalog.h"
#include "Utils.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

// --- Integrity check -------------------------------------------------------------------------------------
//
// Compares the catalog model against what's on disk. Two directions: (A) per entry - is this entry's backing
// intact? (grids below); (B) on-disk content no entry claims - a whole video frame folder, or a stray image
// file under Photos/ - surfaced by the "untracked" walk lower down.
//
// (A) VIDEO  <folder> = frame folder. Cell = verdict; columns are what the folder holds, rows the split flag.
//            preview/ is the card's ONLY render source (both rows); real frames are the deliverable, due once split.
//
//     | split       | frames+preview  | preview only    | frames only     | empty / gone
//     |-------------|-----------------|-----------------|-----------------|-----------------
//     | split=true  | ok              | GHOST           | INVISIBLE       | GHOST+INVISIBLE
//     | split=false | STALE flag      | ok              | STALE+INVISIBLE | INVISIBLE
//
//       GHOST     = real frames gone (source present -> re-import, else remove)
//       INVISIBLE = preview/ gone -> card silently absent from the grid
//       STALE     = real frames exist but entry still marked preview-only
//     + source missing (overlays ANY cell): source unavailable - frames may be intact, but re-extract/re-import
//       is blocked, and a not-yet-split entry can never complete its split.
//
// (A) PHOTO  the file IS the item (no preview, no frames; the card decodes it). Only the source file matters:
//
//     |            | present | missing
//     |------------|---------|-------------------------------------------------
//     | owned      | ok      | LOST - the library's own file (was <root>/Photos/<label>)
//     | referenced | ok      | GONE - external file moved or unmounted
//
// Overlay (optional, any entry): sourcePath exists but no longer matches the recorded MediaId (size differs)
//   -> the source was replaced by a different file. Needs a per-entry stat; not done by default.
//
// Status: all VIDEO verdicts above are implemented (each non-healthy video emits one MediaIssue carrying the
// flags that hold). PHOTO entries emit a PhotoIssue when the source file is missing (owned -> LOST, referenced
// -> GONE). (B) untracked covers both media kinds: a video frame folder no entry claims (UntrackedFolder), and
// an image file under Photos/<label> that no entry claims as its source (UntrackedPhoto).
// ---------------------------------------------------------------------------------------------------------

namespace CatalogIntegrity {

IntegrityReport scan()
{
	Catalog& catalog = Catalog::instance();
	IntegrityReport report;

	// Phases 1 + 2 in one pass over the model: record a per-entry issue for anything non-healthy, and collect the
	// disk facts the untracked walk needs - every entry's folder (to skip claimed video folders) and every entry's
	// source file (to skip claimed photo files). Both keyed by pathComparisonKey so a case difference between the
	// stored and on-disk path still matches.
	QSet<QString> knownFolders;
	QSet<QString> trackedSources;
	for (const auto& [id, entry] : catalog.mediaItems().asKeyValueRange())
	{
		knownFolders.insert(pathComparisonKey(entry.folder));
		if (!entry.sourcePath.isEmpty())
			trackedSources.insert(pathComparisonKey(entry.sourcePath));

		if (entry.type == Catalog::MediaType::Photo)
		{
			// A photo is source-only (the card decodes the file itself) - no frames or preview to probe. Its
			// sole failure is the file going missing: owned -> LOST, referenced -> GONE.
			if (entry.sourcePath.isEmpty() || !QFileInfo::exists(entry.sourcePath))
				report.photoIssues.push_back({ id, entry.sourcePath, entry.referenced });
			continue;
		}

		// Probe the entry's on-disk backing; record a MediaIssue for anything non-healthy. The verdicts
		// (ghost / invisible / stale / source-missing) are orthogonal - see the state grid above.
		MediaIssue issue;
		issue.id                = id;
		issue.folder            = entry.folder;
		issue.sourcePath        = entry.sourcePath;
		issue.sourcePresent     = !entry.sourcePath.isEmpty() && QFileInfo::exists(entry.sourcePath);
		issue.splitComplete     = entry.splitIntoFrames;
		issue.realFramesPresent = !QDir(entry.folder).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty();
		issue.previewPresent    = !QDir(Catalog::previewDirFor(entry.folder)).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty();
		if (!issue.healthy())
			report.issues.push_back(issue);
	}

	// Untracked - on-disk content no entry claims. forEachFolder yields the second level: for a normal collection
	// that's a video frame folder; for the reserved Photos collection it's a <label> dir whose untracked units are
	// the image FILES inside (owned photos), not the folder itself.
	forEachFolder(rootFolder(), [&](const QString& collection, const QString& folderPath) {
		if (collection.compare(PHOTOS_DIR_NAME, Qt::CaseInsensitive) == 0)
		{
			const QString labelName = QFileInfo(folderPath).fileName();
			for (const QFileInfo& file : QDir(folderPath).entryInfoList(IMAGE_FILE_FILTERS, QDir::Files))
				if (!trackedSources.contains(pathComparisonKey(file.absoluteFilePath())))
					report.untrackedPhotos.push_back({ file.absoluteFilePath(), labelName });
			return;
		}
		if (knownFolders.contains(pathComparisonKey(folderPath)))
			return;
		if (QDir(folderPath).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty())
			return;  // an empty/junk dir, not a video the catalog is missing
		report.untracked.push_back({ folderPath });
	});

	return report;
}

} // namespace CatalogIntegrity
