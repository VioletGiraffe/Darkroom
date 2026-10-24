#pragma once

#include "Core/MediaItemId.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

class Catalog;

// UI-free per-item import operations; callers present every returned outcome.
namespace Import {

enum class Status
{
	Success,
	// The output folder exists and overwriteExisting is false; nothing was touched.
	FolderConflict,
	Error,
};

struct Result
{
	Status status = Status::Success;
	QString errorMessage;  // user-presentable; set iff status == Error
	MediaItemId registeredId = MediaItemId::None;  // set iff status == Success
};

// Creates and registers a preview-only video folder at Catalog::frameFolderForNewVideo. stagedPreviewDir contains
// frames directly; when empty or unusable they are extracted afresh. stagedDurationMs is the matching probe result,
// or -1 when unknown. Refuses a video already tracked at videoPath. Registration failure removes the newly created folder.
[[nodiscard]] Result importVideo(Catalog& catalog, const QString& videoPath, const QString& storageFolderPath, const QString& stagedPreviewDir,
	bool overwriteExisting, qint64 stagedDurationMs = -1);

enum class PhotoImportMode { Copy, Move, Reference };

enum class PhotoStatus { Success, Error };

struct PhotoResult
{
	PhotoStatus status = PhotoStatus::Success;
	QString errorMessage;  // user-presentable; set iff status == Error
	MediaItemId registeredId = MediaItemId::None;  // set iff status == Success
};

// An item already tracked at the destination with the same bytes is the result, registering nothing.
// Owned imports auto-rename around other files and adopt an identical untracked destination file.
// Reference mode touches no files; the caller must add its initial label.
[[nodiscard]] PhotoResult importPhoto(Catalog& catalog, const QString& labelPhotoFolder, const QString& photoPath, PhotoImportMode mode);

} // namespace Import
