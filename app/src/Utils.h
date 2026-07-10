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

// Name filters (for QDir::entryList) matching the frame image files this app produces/reads.
extern const QStringList IMAGE_FILE_FILTERS;

// Full paths of the files under `directory` satisfying `filterPredicate` - its immediate children when `recursive`
// is false, the whole subtree when true. Order is filesystem-defined; sort at the call site when it matters. The
// one folder-to-files expansion the drop and scan paths share (pass isSupportedImageFile, isSupportedMediaFile, ...).
[[nodiscard]] QStringList collectFilesInDirectory(const QString& directory, bool recursive,
                                                  const std::function<bool(const QString&)>& filterPredicate);

// Visits every (collection, folderPath) pair under root, in collection-name order.
template <typename F>
void forEachFolder(const QString& root, F&& callback)
{
	const QDir rootDir(root);
	for (const QString& collection : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
	{
		const QDir collDir(rootDir.filePath(collection));
		for (const QString& folder : collDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
			callback(collection, collDir.filePath(folder));
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

[[nodiscard]] bool openInExplorer(const QString& path);

// Modal warning that names a file the app expected to find but didn't. Centralizes the one wording so every
// "the source file is gone" path reports identically and always tells the user which path is missing.
void reportMissingFile(QWidget* parent, const QString& path);

[[nodiscard]] QString ffmpegPath();

[[nodiscard]] QString rootFolder();

// The reserved directory under rootFolder() where owned photos are stored, one subfolder per label:
// <photosRootFolder()>/<label>/<photo file>. Created lazily on first photo import to a label. Because it
// shares the namespace of collection folders, "Photos" is a reserved name - no ordinary label may use it
// (createCollection and Catalog::renameLabel refuse it), or its collection folder would intermingle with
// the photo storage.
extern const QString PHOTOS_DIR_NAME;
[[nodiscard]] QString photosRootFolder();
