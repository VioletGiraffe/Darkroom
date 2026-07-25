#pragma once

#include "Core/MediaId.h"

#include <QString>

#include <vector>

class Catalog;

// Read-only catalog-vs-disk scan. Resolution belongs to Catalog/MainWindow.
namespace CatalogIntegrity {

struct UntrackedFolder
{
	QString folderPath;
};

// Unclaimed image already inside <root>/Photos/<label>.
struct UntrackedPhoto
{
	QString filePath;
	QString labelName;
};

// Raw video disk facts; verdict predicates are orthogonal.
struct MediaIssue
{
	MediaId id;
	QString folder;
	QString sourcePath;
	bool    sourcePresent     = false;
	bool    realFramesPresent = false;
	bool    previewPresent    = false;
	bool    splitComplete     = false;

	[[nodiscard]] bool extractedFramesMissing() const { return splitComplete && !realFramesPresent; }
	[[nodiscard]] bool previewMissing() const         { return !previewPresent; }
	[[nodiscard]] bool splitFlagStale() const         { return !splitComplete && realFramesPresent; }
	[[nodiscard]] bool sourceMissing() const          { return !sourcePresent; }
	[[nodiscard]] bool healthy() const                { return sourcePresent && !extractedFramesMissing() && !previewMissing() && !splitFlagStale(); }
};

// A tracked photo whose only backing file is missing; referenced selects the available recovery.
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

// Explicit, read-only scan; never part of normal refresh.
[[nodiscard]] IntegrityReport scan(const Catalog& catalog, const QString& rootFolder);

} // namespace CatalogIntegrity
