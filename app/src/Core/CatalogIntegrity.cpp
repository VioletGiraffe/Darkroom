#include "Core/CatalogIntegrity.h"
#include "Core/Catalog.h"
#include "Utils.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

// --- Integrity check -------------------------------------------------------------------------------------
//
// Compares the catalog model against what's on disk. Two directions: (A) per entry - is this entry's backing
// intact? (grids below); (B) per folder - on-disk content under no entry? (the "untracked" walk lower down).
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
// -> GONE). Still deferred: (B) untracked covers video frame folders only - an unclaimed file under
// Photos/<label> isn't surfaced yet.
// ---------------------------------------------------------------------------------------------------------

namespace CatalogIntegrity {

IntegrityReport scan()
{
	Catalog& catalog = Catalog::instance();
	IntegrityReport report;

	// Phase 1: per-video-entry issue checks.
	for (const MediaId& id : catalog.allMediaItems())
	{
		const QString source = catalog.sourcePathForMediaItem(id);

		if (catalog.mediaType(id) == Catalog::MediaType::Photo)
		{
			// A photo is source-only (the card decodes the file itself) - no frames or preview to probe. Its
			// sole failure is the file going missing: owned -> LOST, referenced -> GONE.
			if (source.isEmpty() || !QFileInfo::exists(source))
				report.photoIssues.push_back({ id, source, catalog.isReferenced(id) });
			continue;
		}

		const QString folder = catalog.folderForMediaItem(id);

		// Probe the entry's on-disk backing; record a MediaIssue for anything non-healthy. The verdicts
		// (ghost / invisible / stale / source-missing) are orthogonal - see the state grid above.
		MediaIssue issue;
		issue.id                = id;
		issue.folder            = folder;
		issue.sourcePath        = source;
		issue.sourcePresent     = !source.isEmpty() && QFileInfo::exists(source);
		issue.splitComplete     = catalog.isSplitIntoFrames(id);
		issue.realFramesPresent = !QDir(folder).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty();
		issue.previewPresent    = !QDir(Catalog::previewDirFor(folder)).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty();
		if (!issue.healthy())
			report.issues.push_back(issue);
	}

	// Phase 2: collect every entry's frame folder (all types), to exclude them from the untracked walk below.
	// Keyed by pathComparisonKey so a folder whose on-disk case differs from its stored case still matches.
	QSet<QString> knownFolders;
	for (const MediaId& id : catalog.allMediaItems())
		knownFolders.insert(pathComparisonKey(catalog.folderForMediaItem(id)));

	// Phase 3: untracked - every non-empty frame folder on disk that no entry claims.
	forEachFolder(rootFolder(), [&](const QString& collection, const QString& folderPath) {
		if (collection.compare(PHOTOS_DIR_NAME, Qt::CaseInsensitive) == 0)
			return;  // <root>/Photos holds owned photo files (one dir per label), not video frame folders
		if (knownFolders.contains(pathComparisonKey(folderPath)))
			return;
		if (QDir(folderPath).entryList(IMAGE_FILE_FILTERS, QDir::Files).isEmpty())
			return;  // an empty/junk dir, not a video the catalog is missing
		report.untracked.push_back({ folderPath });
	});

	return report;
}

} // namespace CatalogIntegrity
