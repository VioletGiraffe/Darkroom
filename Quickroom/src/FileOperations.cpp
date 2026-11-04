#include "FileOperations.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/OpenFileRelease.h"
#include "Windows/PhotoCompareWindow.h"
#include "Windows/RemovalAction.h"
#include "Windows/VideoPlayerWindow.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "fileoperations/centrypath.h"
#include "fileoperations/cfilesystemmutator.h"
#include "fileoperations/inlinerename.h"
#include "fileoperationsui/cfileoperationdialog.h"
#include "fileoperationsui/fileoperationtexts.h"

DISABLE_COMPILER_WARNINGS
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QObject>
#include <QUrl>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdint.h>
#include <utility>
#include <vector>

namespace {

// Explorer's cut marker: a DWORD drop effect, reached through Qt's custom Windows clipboard formats.
const QString PREFERRED_DROP_EFFECT_FORMAT = QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
constexpr uint32_t MOVE_DROP_EFFECT = 2; // DROPEFFECT_MOVE, without the Windows header defining it
// The KDE and GNOME cut markers; the GNOME one repeats the URLs after a "cut" line.
const QString KDE_CUT_FORMAT = QStringLiteral("application/x-kde-cutselection");
const QString GNOME_COPIED_FILES_FORMAT = QStringLiteral("x-special/gnome-copied-files");

// Deletes one path on behalf of window; remove returns whether it is gone.
[[nodiscard]] RemovalAction deleteAction(QWidget* window, PathDeletion::Mode mode, std::function<void(const QString& path)> onDeleted)
{
	return { FileOperations::deleteActionText(mode), [window, mode, onDeleted = std::move(onDeleted)](const QString& path) {
		const bool deleted = !FileOperations::deleteWithConfirmation({ path }, mode, window).empty();
		if (deleted && onDeleted)
			onDeleted(path);
		return deleted;
	} };
}

// Empty when cancelled. A file's name is preselected up to its extension, as file managers do.
[[nodiscard]] QString askForNewName(const QString& name, bool isFolder, QWidget* parent)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QObject::tr("Rename"));
	dialog.setMinimumWidth(400);

	auto* nameField = new QLineEdit(name);
	const qsizetype stemLength = isFolder ? 0 : QFileInfo(name).completeBaseName().size();
	nameField->setSelection(0, static_cast<int>(stemLength > 0 ? stemLength : name.size()));

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	auto* layout = new QVBoxLayout(&dialog);
	layout->addWidget(new QLabel(QObject::tr("New name:")));
	layout->addWidget(nameField);
	layout->addWidget(buttons);

	return dialog.exec() == QDialog::Accepted ? nameField->text() : QString{};
}

// Players and viewers holding path, or the file it replaces, let go of it during the rename and then follow it to newPath.
[[nodiscard]] InlineRenameResult renameReleasingOpenFiles(const CEntryPath& source, const QString& path, const QString& newName, const QString& newPath, bool replaceExistingFile)
{
	QStringList released{ path };
	if (replaceExistingFile)
		released.push_back(newPath);
	releaseMediaFilesHeldOpen(released);

	InlineRenameResult result = inlineRename(source, newName, replaceExistingFile);
	std::vector<PathMove> completedMoves;
	if (result.status == InlineRenameStatus::Renamed)
		completedMoves.push_back({ path, newPath });
	followMovedMediaFiles(released, completedMoves);
	return result;
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

void FileOperations::putPathsOnClipboardAsText(const QStringList& paths)
{
	if (!paths.empty())
		QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(paths.join('\n')));
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
			QMessageBox::warning(parent, kind == TransferKind::Move ? QObject::tr("Move") : QObject::tr("Copy"), requestValidationErrorText(request.error()));
		return;
	}

	if (kind == TransferKind::Move)
		releaseMediaFilesHeldOpen(sources);

	auto* dialog = new CFileOperationDialog(std::move(*request), {}, parent);
	dialog->start();
}

bool FileOperations::isOnSameVolume(const QString& source, const QString& destinationFolder)
{
	const std::optional<CEntryPath> sourcePath = parseOperationPath(source);
	const std::optional<CEntryPath> folderPath = parseOperationPath(destinationFolder);
	return sourcePath && folderPath && !crossDevicePredicted(*sourcePath, *folderPath);
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

QString FileOperations::renameInteractive(const QString& path, QWidget* dialogParent)
{
	const std::optional<CEntryPath> source = parseOperationPath(path);
	assert_and_return_r(source && !source->isRoot(), {});

	const QString title = QObject::tr("Rename");
	const bool isFolder = QFileInfo(path).isDir();
	// A refused name is offered again for correction.
	for (QString newName = source->name();;)
	{
		newName = askForNewName(newName, isFolder, dialogParent);
		if (newName.isEmpty())
			return {};

		// Built as listFolder builds entry paths, so the caller can find the entry by it.
		const QString newPath = QFileInfo(path).absoluteDir().absoluteFilePath(newName);
		InlineRenameResult result = renameReleasingOpenFiles(*source, path, newName, newPath, false);
		if (result.status == InlineRenameStatus::ReplacementRequired)
		{
			const QString question = QObject::tr("A %1 named “%2” already exists here. Replace it permanently?").arg(fileOperationEntryKindNoun(*result.destinationKind), newName);
			if (QMessageBox::warning(dialogParent, title, question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
				continue;
			result = renameReleasingOpenFiles(*source, path, newName, newPath, true);
		}

		using enum InlineRenameStatus;
		switch (result.status)
		{
		case Renamed:
			return newPath;
		case NothingToDo:
			return {};
		case Rejected:
			QMessageBox::warning(dialogParent, title, QObject::tr("A %1 named “%2” already exists here.").arg(fileOperationEntryKindNoun(*result.destinationKind), newName));
			break;
		case RejectedInvalidName:
			QMessageBox::warning(dialogParent, title, newNameRejectionText(result.nameRejection));
			break;
		case Failed:
			QMessageBox::critical(dialogParent, title, QObject::tr("Could not rename “%1”: %2.").arg(source->name(), fileSystemErrorText(result.failure->filesystemError)));
			return {};
		case ReplacementRequired:
			assert_and_return_unconditional_r("inlineRename asked again after replaceExistingFile", {});
		}
	}
}

QString FileOperations::deleteActionText(PathDeletion::Mode mode)
{
	return mode == PathDeletion::Mode::Trash ? QObject::tr("Move to Trash") : QObject::tr("Delete permanently");
}

void FileOperations::installDeleteActions(ImageViewerWindow& viewer, std::function<void(const QString& path)> onDeleted)
{
	viewer.setRemovalActions(deleteAction(&viewer, PathDeletion::Mode::Trash, onDeleted), deleteAction(&viewer, PathDeletion::Mode::Permanent, onDeleted));
}

void FileOperations::installDeleteActions(VideoPlayerWindow& player, std::function<void(const QString& path)> onDeleted)
{
	player.setRemovalActions(deleteAction(&player, PathDeletion::Mode::Trash, onDeleted), deleteAction(&player, PathDeletion::Mode::Permanent, onDeleted));
}

void FileOperations::installDeleteAction(PhotoCompareWindow& compareWindow, std::function<void(const QString& path)> onDeleted)
{
	compareWindow.setRemovalAction(deleteAction(&compareWindow, PathDeletion::Mode::Trash, std::move(onDeleted)));
}
