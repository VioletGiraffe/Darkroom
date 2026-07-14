#pragma once

#include "Core/MediaId.h"

#include <QString>

#include <vector>

class Catalog;

// The read-only catalog-vs-disk scan and its report vocabulary. The scan (see the state grid above scan() in
// the .cpp) reasons purely over Catalog's public API plus the on-disk layout; the *resolutions* it feeds
// (re-import / regenerate-preview / mark-split / relocate / remove) are mutations that live on Catalog and
// MainWindow, because only they can touch the model, persist, and drive ffmpeg.
namespace CatalogIntegrity {

// A non-empty frame folder on disk that no catalog entry claims. The user resolves it by browsing to its
// source video (which registers it), or skips it.
struct UntrackedFolder
{
	QString folderPath;
};

// An image file in the owned-photos tree (<root>/Photos/<label>/) that no catalog entry claims as its source -
// an orphan. The user adopts it (registers it as an owned photo under <label>) or leaves it. labelName is the
// <label> subfolder it sits in, i.e. the label it would join on adoption.
struct UntrackedPhoto
{
	QString filePath;
	QString labelName;
};

// A tracked video whose on-disk backing is not fully intact, in any combination - the grid's verdicts are
// orthogonal (see scan()'s banner), so several can hold on one entry. Carries the raw disk facts; the
// predicates name the verdicts the dialog renders and resolves. A healthy video -> no issue.
struct MediaIssue
{
	MediaId id;
	QString folder;
	QString sourcePath;
	bool    sourcePresent     = false;  // source file still on disk
	bool    realFramesPresent = false;  // real frames in <folder> - the deliverable
	bool    previewPresent    = false;  // preview frames in Catalog::previewDirFor(<folder>) - the card's only render source
	bool    splitComplete     = false;  // the entry's splitIntoFrames flag

	[[nodiscard]] bool isGhost() const       { return splitComplete && !realFramesPresent; }  // deliverable gone
	[[nodiscard]] bool isInvisible() const   { return !previewPresent; }                      // card can't render
	[[nodiscard]] bool isStale() const       { return !splitComplete && realFramesPresent; }  // flag disagrees with disk
	[[nodiscard]] bool sourceMissing() const { return !sourcePresent; }
	[[nodiscard]] bool healthy() const       { return sourcePresent && !isGhost() && !isInvisible() && !isStale(); }
};

// A tracked photo whose source file is missing. A photo is source-only (no frames, no preview - the card
// decodes the file itself), so a missing file is its whole failure mode: owned -> LOST (the library's own
// <root>/Photos/<label> file is gone), referenced -> GONE (the external file moved or was unmounted). A
// present file -> no issue. referenced selects the verdict wording and which resolutions the dialog offers.
struct PhotoIssue
{
	MediaId id;
	QString sourcePath;
	bool    referenced = false;
};

struct IntegrityReport
{
	std::vector<UntrackedFolder> untracked;
	std::vector<UntrackedPhoto>  untrackedPhotos;
	std::vector<MediaIssue>      issues;
	std::vector<PhotoIssue>      photoIssues;
	[[nodiscard]] bool isEmpty() const { return untracked.empty() && untrackedPhotos.empty() && issues.empty() && photoIssues.empty(); }
};

// Walks the catalog model + disk once (only ever on explicit user request, never part of the normal refresh
// path), looking for the drift the banner above the definition maps. Read-only.
[[nodiscard]] IntegrityReport scan(const Catalog& catalog, const QString& rootFolder);

} // namespace CatalogIntegrity
