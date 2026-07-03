#pragma once

#include "Core/MediaId.h"

#include <QString>

#include <vector>

// The read-only catalog-vs-disk scan and its report vocabulary. The scan (see the state grid above scan() in
// the .cpp) reasons purely over Catalog's public API plus the on-disk layout; the *resolutions* it feeds
// (relink / re-import / regenerate-preview / mark-split / remove) are mutations that live on Catalog and
// MainWindow, because only they can touch the model, persist, and drive ffmpeg.
namespace CatalogIntegrity {

// A placeholder (source was missing when seeded) whose recorded source path now resolves to an existing file -
// the source has reappeared and the item can be relinked to its real identity.
struct RelinkCandidate
{
	MediaId placeholderId;
	QString folder;              // absolute, for display
	QString recordedSourcePath;  // where the source was last known; now exists
};

// A non-empty frame folder on disk that no catalog entry claims. The user resolves it by browsing to its
// source video (which registers it), or skips it.
struct UntrackedFolder
{
	QString folderPath;
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

struct IntegrityReport
{
	std::vector<RelinkCandidate> relinkable;
	std::vector<UntrackedFolder> untracked;
	std::vector<MediaIssue>      issues;
	[[nodiscard]] bool isEmpty() const { return relinkable.empty() && untracked.empty() && issues.empty(); }
};

// Walks the catalog model + disk once (only ever on explicit user request, never part of the normal refresh
// path), looking for the drift the banner above the definition maps. Read-only.
[[nodiscard]] IntegrityReport scan();

} // namespace CatalogIntegrity
