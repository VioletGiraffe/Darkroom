#pragma once

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
// fills its preview/ subfolder (copied from stagedPreviewDir when Quick Import already extracted frames
// there, extracted fresh otherwise) and registers the video in the Catalog with the full frame split
// deferred. A registration refusal (name+size collision with an item tracked elsewhere) deletes the
// just-created folder again and reports as an Error.
[[nodiscard]] Result importVideo(const QString& videoPath, const QString& collectionPath, const QString& stagedPreviewDir, bool overwriteExisting);

} // namespace Import
