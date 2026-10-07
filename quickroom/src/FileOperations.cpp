#include "FileOperations.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
RESTORE_COMPILER_WARNINGS

#include <algorithm>

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
