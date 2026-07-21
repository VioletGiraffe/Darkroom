#pragma once

// <QDir> stays included (not forward-declared): the forEachFolder template below is header-defined and uses QDir
// concretely, so the full type is required here. It also transitively supplies QString/QStringList for the
// declarations below. Everything else this header's implementations need lives in Utils.cpp.
#include <QDir>

#include <functional>

class QDateTime;
class QMimeData;
class QWidget;

void saveWindowGeometry(QWidget* w, const QString& key);
bool restoreWindowGeometry(QWidget* w, const QString& key);

// Re-syncs a QSS :hover highlight that a popup menu's exec() can leave stuck on. :hover matches on the
// Qt::WA_UnderMouse attribute, which Qt's enter/leave dispatch owns and sets separately from the Enter/Leave
// events; the menu's mouse grab bypasses that dispatch, so the widget that spawned the menu keeps the attribute
// set and stays highlighted after the menu closes, looking like a selection. Clear it directly (a synthetic Leave
// event won't - it doesn't touch the attribute) and repaint, but only if the cursor genuinely left the widget, so
// a right-click then Esc without moving the mouse keeps a legitimate hover. See docs/tips/qt-styling-system-quirks.md.
void clearStuckHoverIfCursorLeft(QWidget* w);

// Picks up to maxFrames evenly spaced entries from files (assumed sorted) and
// returns them as full paths under dir. Frames are sampled between startOffset
// and endOffset (as fractions of the total count) to avoid uninformative
// black frames at the very start/end of a clip.
[[nodiscard]] QStringList pickEvenlySpacedFrames(const QDir& dir, const QStringList& files, int maxFrames);

[[nodiscard]] bool isSupportedVideoFile(const QString& filePath);
[[nodiscard]] bool isSupportedImageFile(const QString& filePath);
// Either kind of media the app ingests - a supported video or a supported photo. The two predicates above stay
// distinct (import routes videos and photos differently); this is the "is it either" test the walk/drop paths want.
[[nodiscard]] bool isSupportedMediaFile(const QString& filePath);

// Useful for drop handlers that accept directories or files
[[nodiscard]] bool isDirectoryOrSupportedFile(const QString& path);

// Returns the list paths from urls() for which isDirectoryOrSupportedFile() returns true
[[nodiscard]] bool hasSupportedPaths(const QMimeData* mime);
[[nodiscard]] QStringList supportedPaths(const QMimeData* mime);

// Byte-for-byte comparison of two files; the size check short-circuits the common case where the
// names collide but the content doesn't.
[[nodiscard]] bool filesAreIdentical(const QString& pathA, const QString& pathB);

// Case/separator-insensitive key for testing whether two paths refer to the same location when they may come
// from different sources (e.g. a catalog-stored path vs. one freshly walked off disk). Canonical form + lower case. Not meant for storing/directly using, but in practice fine for that too
[[nodiscard]] QString pathComparisonKey(const QString& path);

// Rejects characters illegal in Windows file/folder names: returns the first offending character, or a null
// QChar (isNull()) for a clean name. For validating user-typed names in the rename flows.
[[nodiscard]] QChar invalidFilenameChar(const QString& name);

// Glob filters matching the frame image files this app produces/reads - for QFileDialog filter strings.
extern const QStringList IMAGE_FILE_FILTERS;

// The frame image files directly inside dir (same formats as IMAGE_FILE_FILTERS), name-sorted. Use this for
// directory scans instead of entryList(IMAGE_FILE_FILTERS): it suffix-matches case-insensitively, so ".JPG"
// files are found on a case-sensitive filesystem too.
[[nodiscard]] QStringList listFrameImageFiles(const QDir& dir);

// Full paths of the files under `directory` satisfying `filterPredicate` - its immediate children when `recursive`
// is false, the whole subtree when true. Order is filesystem-defined; sort at the call site when it matters. The
// one folder-to-files expansion the drop and scan paths share (pass isSupportedImageFile, isSupportedMediaFile, ...).
[[nodiscard]] QStringList collectFilesInDirectory(const QString& directory, bool recursive,
                                                  const std::function<bool(const QString&)>& filterPredicate);

// Visits every (storageFolder, folderPath) pair under root, in storage-folder-name order.
template <typename F>
void forEachFolder(const QString& root, F&& callback)
{
	const QDir rootDir(root);
	for (const QString& storageFolder : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
	{
		const QDir storageDir(rootDir.filePath(storageFolder));
		for (const QString& folder : storageDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
			callback(storageFolder, storageDir.filePath(folder));
	}
}

// Parses a recording timestamp from the end of `text`, as in "44-20260609074824828398":
// an arbitrary prefix followed by a trailing run of digits whose first 14 digits are
// yyyyMMddHHmmss (any further trailing digits - sub-second precision, etc. - are ignored).
// Returns an invalid QDateTime if `text` doesn't end this way. The caller passes whatever
// string carries the timestamp (e.g. a folder name); to parse a file name, strip its
// extension first - this function deliberately does no extension handling of its own.
[[nodiscard]] QDateTime parseTrailingTimestamp(const QString& text);

// Best-effort recording date of the source video a frame folder was extracted from - a "video
// date", deliberately not the folder's own creation time (that's the import date). Prefers the
// timestamp embedded in the source file's name (see parseTrailingTimestamp), which survives the file
// being moved; falls back to the video file's creation time, and only as a last resort - when the
// source video isn't recorded or no longer exists - to the folder's own timestamp. The caller supplies
// both the source path (from the catalog) and the frame folder for the last-resort fallback.
[[nodiscard]] QDateTime getSourceFileDate(const QString& sourcePath, const QString& folderPath);

// Shows `path` in the platform's file manager, selected within its parent folder: Explorer's /select, Finder's
// Reveal, the freedesktop ShowItems call. Returns false - having done nothing - only when `path` doesn't exist, so
// a false return always means exactly that (what every caller reports via reportMissingFile). Launching the file
// manager is best-effort past that point and deliberately not folded into the return value.
[[nodiscard]] bool revealInFileManager(const QString& path);

// Menu text for a revealInFileManager action, in the platform's own idiom. Only for call sites that name the file
// manager; wordings that don't ("Locate source file") work everywhere as-is and shouldn't use this.
[[nodiscard]] QString revealInFileManagerActionText();

// Modal warning that names a file the app expected to find but didn't. Centralizes the one wording so every
// "the source file is gone" path reports identically and always tells the user which path is missing.
void reportMissingFile(QWidget* parent, const QString& path);

namespace Ffmpeg { struct SplitResult; }

// Modal rendering of a failed Ffmpeg::splitVideoIntoFrames / extractFrame outcome, one wording per status.
// outputTarget is the folder the extraction was writing into, named in the folder-creation failure. No-op on Ok.
void reportFfmpegFailure(QWidget* parent, const Ffmpeg::SplitResult& result, const QString& videoFilePath, const QString& outputTarget);

// Absolute path of the ffmpeg binary to run, or empty when it can't be found at all: the configured setting when it
// names an existing file, else autoDetectedFfmpegPath(). An empty return needs no guarding at the call site:
// QProcess reports it as FailedToStart, which the Ffmpeg module already surfaces as StartFailed.
[[nodiscard]] QString ffmpegPath();

// Where ffmpeg is found without having been told: beside the app, on PATH, or - macOS only - the directories
// Homebrew and MacPorts install into (a Finder-launched app's PATH holds neither). Empty when none of them has it.
// This is for showing the user what an unconfigured setting resolves to; to actually run ffmpeg call ffmpegPath(),
// which honours the setting first.
[[nodiscard]] QString autoDetectedFfmpegPath();
