#include "FileOperations.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/OpenFileRelease.h"
#include "Windows/PhotoCompareWindow.h"
#include "Windows/RemovalAction.h"
#include "Windows/VideoPlayerWindow.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "fileoperations/centrypath.h"
#include "fileoperations/cfilesystemmutator.h"
#include "fileoperations/entrycreation.h"
#include "fileoperations/fileoperationtexts.h"
#include "fileoperations/inlinerename.h"
#include "fileoperationsui/cfileoperationdialog.h"
#include "fileoperationsui/usermessagebox.h"

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
[[nodiscard]] QString askForName(const QString& title, const QString& prompt, const QString& name, bool isFolder, QWidget* parent)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(title);
	dialog.setMinimumWidth(400);

	auto* nameField = new QLineEdit(name);
	const qsizetype stemLength = isFolder ? 0 : QFileInfo(name).completeBaseName().size();
	nameField->setSelection(0, static_cast<int>(stemLength > 0 ? stemLength : name.size()));

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	auto* layout = new QVBoxLayout(&dialog);
	layout->addWidget(new QLabel(prompt));
	layout->addWidget(nameField);
	layout->addWidget(buttons);

	return dialog.exec() == QDialog::Accepted ? nameField->text() : QString{};
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

	const bool isFolder = QFileInfo(path).isDir();
	// A refused name is offered again for correction.
	for (QString newName = source->name();;)
	{
		newName = askForName(QObject::tr("Rename"), QObject::tr("New name:"), newName, isFolder, dialogParent);
		if (newName.isEmpty())
			return {};

		InlineRenameResult result = renameEntryReleasingOpenFiles(*source, newName, false);
		if (result.status == InlineRenameStatus::ReplacementRequired)
		{
			const UserMessage question = inlineRenameReplacementQuestion(result, newName);
			if (QMessageBox::warning(dialogParent, question.title, question.text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
				continue;
			result = renameEntryReleasingOpenFiles(*source, newName, true);
		}

		// Built as listFolder builds entry paths, so the caller can find the entry by it.
		if (result.status == InlineRenameStatus::Renamed)
			return QFileInfo(path).absoluteDir().absoluteFilePath(newName);
		if (result.status == InlineRenameStatus::NothingToDo)
			return {};

		const std::optional<UserMessage> refusal = inlineRenameMessage(result, source->name(), newName);
		assert_and_return_r(refusal, {}); // Empty only for ReplacementRequired, which replaceExistingFile rules out
		showUserMessage(dialogParent, *refusal);
		if (result.status == InlineRenameStatus::Failed)
			return {};
	}
}

QString FileOperations::createFolderInteractive(const QString& parentFolder, QWidget* dialogParent)
{
	const std::optional<CEntryPath> parent = parseOperationPath(parentFolder);
	assert_and_return_r(parent, {});

	const QDir parentDir(parentFolder);
	QString name = QObject::tr("New folder");
	for (int number = 2; parentDir.exists(name); ++number)
		name = QObject::tr("New folder (%1)").arg(number);

	// A refused or taken name is offered again for correction.
	for (;;)
	{
		name = askForName(QObject::tr("New folder"), QObject::tr("Folder name (a/b creates nested folders):"), name, true, dialogParent);
		if (name.isEmpty())
			return {};

		const EntryCreationResult result = createNewFolder(*parent, name);
		// Built as listFolder builds entry paths, so the caller can find the entry by it.
		if (result.status == EntryCreationStatus::Created)
			return parentDir.absoluteFilePath(result.createdInParent->name());

		showUserMessage(dialogParent, *entryCreationMessage(result, name));
		if (result.status != EntryCreationStatus::AlreadyExists && result.status != EntryCreationStatus::RejectedInvalidName)
			return {};
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
