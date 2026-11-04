#include "Windows/MediaRename.h"
#include "Core/Catalog.h"
#include "Windows/OpenFileRelease.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "fileoperations/centrypath.h"
#include "fileoperations/inlinerename.h"
#include "fileoperations/newnamecheck.h"
#include "fileoperationsui/fileoperationtexts.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <vector>

namespace {

using MediaRename::Result;

// Renames from to to, a path in the same folder, never replacing an existing entry. Returns why it failed, empty on success.
[[nodiscard]] QString renameEntry(const QString& from, const QString& to)
{
	const std::optional<CEntryPath> source = parseOperationPath(from);
	assert_and_return_r(source && !source->isRoot(), QObject::tr("the path is not valid"));

	return inlineRenameFailureText(inlineRename(*source, QFileInfo(to).fileName(), false));
}

// Warns and returns true when name cannot be a file or folder name on every platform: a library may move between them.
[[nodiscard]] bool rejectUnusableName(const QString& name, QWidget* parent, const QString& title)
{
	const NameRejection rejection = checkNewEntryName(name, NameRules::Portable);
	if (rejection == NameRejection::None)
		return false;

	QMessageBox::warning(parent, title, newNameRejectionText(rejection));
	return true;
}

// Moves back newest first. Returns a description of the moves that could not be undone, which stay in moves.
[[nodiscard]] QString undoCompletedMoves(std::vector<PathMove>& moves)
{
	QString failures;
	std::vector<PathMove> notUndone;
	for (auto move = moves.crbegin(); move != moves.crend(); ++move)
	{
		if (renameEntry(move->to, move->from).isEmpty())
			continue;
		failures += "\n\n" + QObject::tr("Additionally, this rename could not be undone:\n%1\n→ %2").arg(move->to, move->from);
		notUndone.push_back(*move);
	}
	moves = std::move(notUndone);
	return failures;
}

// Rename disk objects before the catalog, rolling them back if a later step fails.
Result renameVideo(Catalog& catalog, MediaItemId id, const QString& newName, const QString& newSourcePath, const QString& newFolderPath, QWidget* parent)
{
	const QString oldFolderPath = catalog.folderForMediaItem(id);
	const QString oldSourcePath = catalog.sourcePathForMediaItem(id);
	const bool renameSourceFile = !oldSourcePath.isEmpty() && QFile::exists(oldSourcePath);

	const QStringList movingPaths{ oldSourcePath, oldFolderPath };
	std::vector<PathMove> completedMoves;
	releaseMediaFilesHeldOpen(movingPaths);
	const QString error = [&]() -> QString {
		if (renameSourceFile)
		{
			if (const QString failure = renameEntry(oldSourcePath, newSourcePath); !failure.isEmpty())
				return QObject::tr("Failed to rename the source file: %1\n%2\n→ %3").arg(failure, oldSourcePath, newSourcePath);
			completedMoves.push_back({ oldSourcePath, newSourcePath });
		}

		if (const QString failure = renameEntry(oldFolderPath, newFolderPath); !failure.isEmpty())
			return QObject::tr("Failed to rename the frame folder: %1\n%2\n→ %3").arg(failure, oldFolderPath, newFolderPath) + undoCompletedMoves(completedMoves);
		completedMoves.push_back({ oldFolderPath, newFolderPath });

		if (!catalog.updateMediaItemPaths(id, newSourcePath, newFolderPath))
			return QObject::tr("Another library item is already recorded at:\n%1").arg(newSourcePath) + undoCompletedMoves(completedMoves);
		return {};
	}();
	followMovedMediaFiles(movingPaths, completedMoves);

	if (!error.isEmpty())
	{
		QMessageBox::critical(parent, QObject::tr("Rename media file"), error);
		return {};
	}
	return { .renamed = true, .oldFolderPath = oldFolderPath, .newFolderPath = newFolderPath, .newName = newName };
}

Result renameVideoInteractive(Catalog& catalog, MediaItemId id, QWidget* parent)
{
	assert_r(catalog.mediaType(id) == Catalog::MediaType::Video);

	const QString originalFolderPath = catalog.folderForMediaItem(id);

	if (originalFolderPath.isEmpty() || !QDir(originalFolderPath).exists())
	{
		QMessageBox::critical(parent, QObject::tr("Rename media file"), QObject::tr("Frame folder does not exist:\n%1").arg(originalFolderPath));
		return {};
	}

	const QString oldSourcePath = catalog.sourcePathForMediaItem(id);
	const bool sourceExists = !oldSourcePath.isEmpty() && QFile::exists(oldSourcePath);

	const QString oldBaseName = catalog.displayName(id);
	const QString parentPath  = QFileInfo(originalFolderPath).absolutePath();

	const QString newBaseName = QInputDialog::getText(parent, QObject::tr("Rename media file"), QObject::tr("New name:"), QLineEdit::Normal, oldBaseName).trimmed();

	if (newBaseName.isEmpty() || newBaseName == oldBaseName)
		return {};

	// Skips the early existence checks: the new spelling may name the item's own file and folder, and renameEntry refuses a distinct entry.
	const bool caseChangeOnly = newBaseName.compare(oldBaseName, Qt::CaseInsensitive) == 0;

	const QFileInfo oldSourceInfo{ oldSourcePath };
	QString newSourcePath;
	if (!oldSourcePath.isEmpty())
	{
		const QString newFileName = oldSourceInfo.suffix().isEmpty() ? newBaseName : newBaseName + "." + oldSourceInfo.suffix();
		if (rejectUnusableName(newFileName, parent, QObject::tr("Rename media file")))
			return {};
		newSourcePath = oldSourceInfo.absolutePath() + "/" + newFileName;

		if (!caseChangeOnly && sourceExists && QFile::exists(newSourcePath))
		{
			QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("A file with that name already exists:\n%1").arg(newSourcePath));
			return {};
		}
	}

	const qint64 sourceSize = catalog.mediaItems().value(id).sourceSize;
	const QString newFolderName = Catalog::frameFolderName(newBaseName, oldSourceInfo.suffix(), sourceSize);
	if (rejectUnusableName(newFolderName, parent, QObject::tr("Rename media file")))
		return {};
	const QString newFolderPath = parentPath + "/" + newFolderName;
	if (!caseChangeOnly && QDir(newFolderPath).exists())
	{
		QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("A folder with that name already exists:\n%1").arg(newFolderPath));
		return {};
	}

	QString message = QObject::tr("Rename “%1” to “%2”?\n\n").arg(oldBaseName, newBaseName);
	if (sourceExists)
	{
		message += QObject::tr("• Source file:\n  %1\n  → %2\n\n").arg(oldSourcePath, newSourcePath);
	}
	else if (!oldSourcePath.isEmpty())
	{
		message += QObject::tr("• Source file not found at stored path — it will not be renamed.\n"
			"  The stored path will be updated to reflect the new name.\n\n");
	}
	message += QObject::tr("• Frame folder:\n  %1\n  → %2").arg(originalFolderPath, newFolderPath);

	if (QMessageBox::question(parent, QObject::tr("Rename media file"), message,
		QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		return {};

	return renameVideo(catalog, id, newBaseName, newSourcePath, newFolderPath, parent);
}

Result renamePhoto(Catalog& catalog, MediaItemId id, const QString& newSourcePath, QWidget* parent)
{
	const QString oldSourcePath = catalog.sourcePathForMediaItem(id);
	const QString folderAbs     = catalog.folderForMediaItem(id);

	const QStringList movingPaths{ oldSourcePath };
	std::vector<PathMove> completedMoves;
	releaseMediaFilesHeldOpen(movingPaths);
	const QString error = [&]() -> QString {
		if (const QString failure = renameEntry(oldSourcePath, newSourcePath); !failure.isEmpty())
			return QObject::tr("Failed to rename the file: %1\n%2\n→ %3").arg(failure, oldSourcePath, newSourcePath);
		completedMoves.push_back({ oldSourcePath, newSourcePath });

		if (!catalog.updateMediaItemPaths(id, newSourcePath, folderAbs))
			return QObject::tr("Another library item is already recorded at:\n%1").arg(newSourcePath) + undoCompletedMoves(completedMoves);
		return {};
	}();
	followMovedMediaFiles(movingPaths, completedMoves);

	if (!error.isEmpty())
	{
		QMessageBox::critical(parent, QObject::tr("Rename photo"), error);
		return {};
	}
	return { .renamed = true };
}

Result renamePhotoInteractive(Catalog& catalog, MediaItemId id, QWidget* parent)
{
	assert_r(catalog.mediaType(id) == Catalog::MediaType::Photo);

	const QString title = QObject::tr("Rename photo");
	const QString oldSourcePath = catalog.sourcePathForMediaItem(id);

	if (oldSourcePath.isEmpty() || !QFile::exists(oldSourcePath))
	{
		QMessageBox::warning(parent, title, QObject::tr("The photo file no longer exists, so it cannot be renamed:\n%1").arg(oldSourcePath));
		return {};
	}

	const QFileInfo oldInfo{ oldSourcePath };
	const QString oldBaseName = oldInfo.completeBaseName();
	const QString suffix      = oldInfo.suffix();

	const QString newBaseName = QInputDialog::getText(parent, title, QObject::tr("New name:"), QLineEdit::Normal, oldBaseName).trimmed();
	if (newBaseName.isEmpty() || newBaseName == oldBaseName)
		return {};

	const QString newFileName = suffix.isEmpty() ? newBaseName : newBaseName + "." + suffix;
	if (rejectUnusableName(newFileName, parent, title))
		return {};
	const QString newSourcePath = oldInfo.absolutePath() + "/" + newFileName;

	// Skips the early existence check: the new spelling may name the photo's own file, and renameEntry refuses a distinct entry.
	const bool caseChangeOnly = newBaseName.compare(oldBaseName, Qt::CaseInsensitive) == 0;

	if (!caseChangeOnly && QFile::exists(newSourcePath))
	{
		QMessageBox::warning(parent, title, QObject::tr("A file with that name already exists:\n%1").arg(newSourcePath));
		return {};
	}

	QString message = QObject::tr("Rename “%1” to “%2”?\n\n").arg(oldInfo.fileName(), newFileName);
	message += catalog.isReferenced(id)
		? QObject::tr("• This photo is referenced in place - its file below, outside the library, will be renamed:\n  %1\n  → %2").arg(oldSourcePath, newSourcePath)
		: QObject::tr("• File:\n  %1\n  → %2").arg(oldSourcePath, newSourcePath);

	if (QMessageBox::question(parent, title, message, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		return {};

	return renamePhoto(catalog, id, newSourcePath, parent);
}

} // namespace

MediaRename::Result MediaRename::renameItemInteractive(Catalog& catalog, MediaItemId id, QWidget* dialogParent)
{
	if (catalog.mediaType(id) == Catalog::MediaType::Photo)
		return renamePhotoInteractive(catalog, id, dialogParent);
	return renameVideoInteractive(catalog, id, dialogParent);
}
