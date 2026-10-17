#include "FileOperations.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/OpenFileRelease.h"
#include "Windows/VideoPlayerWindow.h"
#include "compiler/compiler_warnings_control.h"
#include "fileoperationsui/cfileoperationdialog.h"

DISABLE_COMPILER_WARNINGS
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QMimeData>
#include <QObject>
#include <QUrl>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cstring>
#include <stdint.h>
#include <utility>

namespace {

// Explorer's cut marker: a DWORD drop effect, reached through Qt's custom Windows clipboard formats.
const QString PREFERRED_DROP_EFFECT_FORMAT = QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
constexpr uint32_t MOVE_DROP_EFFECT = 2; // DROPEFFECT_MOVE, without the Windows header defining it
// The KDE and GNOME cut markers; the GNOME one repeats the URLs after a "cut" line.
const QString KDE_CUT_FORMAT = QStringLiteral("application/x-kde-cutselection");
const QString GNOME_COPIED_FILES_FORMAT = QStringLiteral("x-special/gnome-copied-files");

[[nodiscard]] QString transferRequestErrorText(RequestValidationError error)
{
	switch (error)
	{
	case RequestValidationError::NoSources:
		return QObject::tr("No valid items are selected for the operation.");
	case RequestValidationError::InvalidPath:
		return QObject::tr("The source or destination path is not a valid absolute path.");
	case RequestValidationError::RootSource:
		return QObject::tr("A drive root cannot be copied, moved, or deleted.");
	case RequestValidationError::ExactEntryRequiresSingleSource:
		return QObject::tr("An exact destination entry can only be chosen for a single source.");
	case RequestValidationError::DestinationInsideSource:
		return QObject::tr("The destination cannot be inside a selected source.");
	}
	return {};
}

// The returned function deletes one path on behalf of window and returns whether it is gone.
[[nodiscard]] std::function<bool(const QString& path)> pathDeleter(QWidget* window, PathDeletion::Mode mode, std::function<void(const QString& path)> onDeleted)
{
	return [window, mode, onDeleted = std::move(onDeleted)](const QString& path) {
		const bool deleted = !FileOperations::deleteWithConfirmation({ path }, mode, window).empty();
		if (deleted && onDeleted)
			onDeleted(path);
		return deleted;
	};
}

} // namespace

void FileOperations::putPathsOnClipboard(const QStringList& paths, bool cut)
{
	if (paths.empty())
		return;

	QList<QUrl> urls;
	for (const QString& path : paths)
		urls.push_back(QUrl::fromLocalFile(path));

	auto* mime = new QMimeData();
	mime->setUrls(urls);
	if (cut)
	{
#ifdef Q_OS_WIN
		const uint32_t dropEffect = MOVE_DROP_EFFECT;
		mime->setData(PREFERRED_DROP_EFFECT_FORMAT, QByteArray(reinterpret_cast<const char*>(&dropEffect), sizeof(dropEffect)));
#else
		mime->setData(KDE_CUT_FORMAT, "1");
		QByteArray gnomeCopiedFiles = "cut";
		for (const QUrl& url : std::as_const(urls))
			gnomeCopiedFiles += '\n' + url.toEncoded();
		mime->setData(GNOME_COPIED_FILES_FORMAT, gnomeCopiedFiles);
#endif
	}
	QGuiApplication::clipboard()->setMimeData(mime);
}

QStringList FileOperations::localFilePaths(const QMimeData& mime)
{
	QStringList paths;
	for (const QUrl& url : mime.urls())
	{
		if (url.isLocalFile())
			paths.push_back(url.toLocalFile());
	}
	return paths;
}

bool FileOperations::isMarkedAsCut(const QMimeData& mime)
{
	if (const QByteArray dropEffect = mime.data(PREFERRED_DROP_EFFECT_FORMAT); dropEffect.size() >= static_cast<qsizetype>(sizeof(uint32_t)))
	{
		uint32_t effect = 0;
		std::memcpy(&effect, dropEffect.constData(), sizeof(effect));
		return (effect & MOVE_DROP_EFFECT) != 0;
	}

	if (mime.data(KDE_CUT_FORMAT) == "1")
		return true;
	const QByteArray gnomeCopiedFiles = mime.data(GNOME_COPIED_FILES_FORMAT);
	return gnomeCopiedFiles == "cut" || gnomeCopiedFiles.startsWith("cut\n");
}

void FileOperations::startTransfer(TransferKind kind, const QStringList& sources, const QString& destinationFolder, QWidget* parent)
{
	auto request = makeTransferRequest(kind, sources, DestinationIntent::IntoDirectory, destinationFolder);
	if (!request)
	{
		if (request.error() != RequestValidationError::NoSources)
			QMessageBox::warning(parent, kind == TransferKind::Move ? QObject::tr("Move") : QObject::tr("Copy"), transferRequestErrorText(request.error()));
		return;
	}

	if (kind == TransferKind::Move)
		releaseMediaFilesHeldOpen(sources);

	auto* dialog = new CFileOperationDialog(std::move(*request), {}, parent);
	dialog->start();
}

QSet<QString> FileOperations::deleteWithConfirmation(const QStringList& paths, PathDeletion::Mode mode, QWidget* dialogParent)
{
	if (paths.empty())
		return {};

	const bool permanent = mode == PathDeletion::Mode::Permanent;
	const bool anyFolder = std::any_of(paths.begin(), paths.end(), [](const QString& path) { return QFileInfo(path).isDir(); });
	if (permanent || anyFolder)
	{
		QString message;
		if (paths.size() == 1)
			message = permanent ? QObject::tr("Permanently delete this item? This cannot be undone.") : QObject::tr("Move this folder to Trash?");
		else
			message = (permanent ? QObject::tr("Permanently delete %1 items? This cannot be undone.") : QObject::tr("Move %1 items to Trash?")).arg(paths.size());
		message += "\n" + PathDeletion::bulletedPathList(paths);
		if (anyFolder)
			message += "\n\n" + QObject::tr("Folders are deleted with all their contents.");

		if (QMessageBox::warning(dialogParent, QObject::tr("Delete"), message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return {};
	}

	return PathDeletion::removePathsInteractive(paths, mode, dialogParent);
}

QString FileOperations::deleteActionText(PathDeletion::Mode mode)
{
	return mode == PathDeletion::Mode::Trash ? QObject::tr("Move to Trash") : QObject::tr("Delete permanently");
}

void FileOperations::installDeleteActions(ImageViewerWindow& viewer, const QStringList& imagePaths, std::function<void(const QString& path)> onDeleted)
{
	const auto removalAction = [&viewer, &imagePaths, &onDeleted](PathDeletion::Mode mode) {
		return ImageViewerWindow::RemovalAction{ deleteActionText(mode),
			[imagePaths, deletePath = pathDeleter(&viewer, mode, onDeleted)](int index) { return deletePath(imagePaths[index]); } };
	};
	viewer.setRemovalActions(removalAction(PathDeletion::Mode::Trash), removalAction(PathDeletion::Mode::Permanent));
}

void FileOperations::installDeleteActions(VideoPlayerWindow& player, std::function<void(const QString& path)> onDeleted)
{
	const auto removalAction = [&player, &onDeleted](PathDeletion::Mode mode) {
		return VideoPlayerWindow::RemovalAction{ deleteActionText(mode),
			[deletePath = pathDeleter(&player, mode, onDeleted)](const MediaId&, const QString& videoPath) { return deletePath(videoPath); } };
	};
	player.setRemovalActions(removalAction(PathDeletion::Mode::Trash), removalAction(PathDeletion::Mode::Permanent));
}
