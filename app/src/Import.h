#pragma once

#include "Core/MediaId.h"

#include <QString>

// The per-item import worker: registers one source video file into a collection. UI-free by design -
// nothing here prompts or reports; every outcome is returned for the caller to present. MainWindow::processBatch
// is the batch coordinator on top of this and owns all the import UI (the app-wide processing lock, the
// progress modal, folder-conflict partitioning/prompts, error boxes, catalog write batching, view refresh).
namespace Import {

enum class Status
{
	Success,
	// The output folder already exists and overwriteExisting was false. Nothing was touched - the caller
	// resolves the conflict (typically by asking the user) and retries with overwriteExisting = true.
	FolderConflict,
	Error,
};

struct Result
{
	Status status = Status::Success;
	QString errorMessage;  // user-presentable; set iff status == Error
};

// Creates <collectionPath>/<video base name>/ (wiping a pre-existing one when overwriteExisting allows it),
// fills its preview/ subfolder (copied from stagedPreviewDir when the Import dialog already extracted frames
// there, extracted fresh otherwise) and registers the video in the Catalog with the full frame split
// deferred. A registration refusal (name+size collision with an item tracked elsewhere) deletes the
// just-created folder again and reports as an Error.
// stagedDurationMs is the duration the Import dialog already probed while staging this video (-1 when unknown, e.g.
// a direct import that reuses no staged frames); it's recorded on the item, saving a redundant probe. The
// fresh-extraction fallback probes anyway, so its result supersedes a -1 here.
[[nodiscard]] Result importVideo(const QString& videoPath, const QString& collectionPath, const QString& stagedPreviewDir, bool overwriteExisting, qint64 stagedDurationMs = -1);

// How a photo enters the library: its file copied or moved into <root>/Photos/<label>/ (owned), or left
// where it is and merely tracked (referenced).
enum class PhotoImportMode { Copy, Move, Reference };

enum class PhotoStatus
{
	Success,
	// Reference mode only: the file's name+size id is already tracked as a different item, and a referenced
	// photo has no file of its own to rename the collision away on. Nothing was registered - the caller may
	// offer importing an owned copy instead (the Copy path auto-renames, resolving the collision).
	IdCollision,
	Error,
};

struct PhotoResult
{
	PhotoStatus status = PhotoStatus::Success;
	QString errorMessage;  // user-presentable; set iff status == Error
	// The identity actually registered (valid iff Success). After an owned-import auto-rename this differs
	// from the source file's own id - callers keying per-item state on the staged id must re-key to this.
	MediaId registeredId;
};

// Imports one photo under the given label. Owned modes copy/move the file into <root>/Photos/<label>/
// (created lazily), auto-renaming the incoming file (name_2.ext, name_3.ext, ...) when its name collides on
// disk or its name+size id collides with a differently-stored catalog item - one rename resolves both. A
// byte-identical file already at the destination is adopted as-is instead of copied again. Reference mode
// touches no files: it registers the photo at photoPath with the referenced flag; the caller applies the
// initial label afterwards (a referenced photo has no storage folder to derive it from).
[[nodiscard]] PhotoResult importPhoto(const QString& photoPath, const QString& labelDisplayName, PhotoImportMode mode);

} // namespace Import
