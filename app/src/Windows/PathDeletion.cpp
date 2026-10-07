#include "Windows/PathDeletion.h"
#include "Windows/VideoPlayerWindow.h"

#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagedialog.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>

namespace {

[[nodiscard]] bool pathExistsOrIsSymlink(const QString& path)
{
	const QFileInfo info(path);
	return info.exists() || info.isSymLink();
}

bool permanentlyRemovePath(const QString& path, QString* error)
{
	const QFileInfo info(path);
	if (!info.exists() && !info.isSymLink())
		return true;

	if (info.isDir() && !info.isSymLink())
	{
		if (QDir(path).removeRecursively())
			return true;
		if (error)
			*error = QObject::tr("The folder could not be removed.");
		return false;
	}

	QFile file(path);
	if (file.remove())
		return true;
	if (error)
		*error = file.errorString().trimmed();
	return false;
}

// singleTrashError is shown only when untrashable holds one path.
[[nodiscard]] bool confirmPermanentDeletionOfUntrashable(const QStringList& untrashable, const QString& singleTrashError, QWidget* dialogParent)
{
	QString text;
	if (untrashable.size() == 1)
	{
		text = QObject::tr("This item could not be moved to Trash:\n\n%1\n\nPermanently delete it instead? This cannot be undone.")
			.arg(QDir::toNativeSeparators(untrashable.front()));
		if (!singleTrashError.isEmpty())
			text += "\n\n" + singleTrashError;
	}
	else
	{
		text = QObject::tr("%1 items could not be moved to Trash:\n%2\n\nPermanently delete them instead? This cannot be undone.")
			.arg(untrashable.size()).arg(PathDeletion::bulletedPathList(untrashable));
	}

	QMessageBox fallback(QMessageBox::Warning, QObject::tr("Move to Trash failed"), text, QMessageBox::NoButton, dialogParent);
	QPushButton* permanentlyDelete = fallback.addButton(QObject::tr("Delete Permanently"), QMessageBox::DestructiveRole);
	QPushButton* cancel = fallback.addButton(QMessageBox::Cancel);
	fallback.setDefaultButton(cancel);
	fallback.setEscapeButton(cancel);
	fallback.exec();
	return fallback.clickedButton() == permanentlyDelete;
}

} // namespace

QString PathDeletion::bulletedPathList(const QStringList& paths)
{
	QString list;
	constexpr qsizetype maxListed = 15;
	for (qsizetype i = 0; i < std::min(maxListed, paths.size()); ++i)
		list += "\n• " + QDir::toNativeSeparators(paths[i]);
	if (paths.size() > maxListed)
		list += "\n" + QObject::tr("... and %1 more").arg(paths.size() - maxListed);
	return list;
}

QSet<QString> PathDeletion::removePathsInteractive(const QStringList& paths, Mode mode, QWidget* dialogParent)
{
	// Windows refuses to delete a file that is open.
	const VideoPlayerWindow::FileRelease playersLetGo{ paths };

	QSet<QString> removed;
	QStringList failures;
	const auto removePermanently = [&removed, &failures](const QString& path) {
		QString error;
		if (permanentlyRemovePath(path, &error))
			removed.insert(path);
		else
			failures.push_back(QDir::toNativeSeparators(path) + (error.isEmpty() ? QString() : "\n" + error));
	};

	QStringList untrashable;
	QString lastTrashError;
	for (const QString& path : paths)
	{
		if (path.isEmpty())
			continue;

		if (mode == Mode::Permanent)
		{
			removePermanently(path);
			continue;
		}

		QFile file(path);
		if (!pathExistsOrIsSymlink(path) || file.moveToTrash())
			removed.insert(path);
		else
		{
			untrashable.push_back(path);
			lastTrashError = file.errorString().trimmed();
		}
	}

	if (!untrashable.empty() && confirmPermanentDeletionOfUntrashable(untrashable, lastTrashError, dialogParent))
	{
		for (const QString& path : std::as_const(untrashable))
			removePermanently(path);
	}

	if (!failures.empty())
	{
		MessageDialog::notice(dialogParent, QObject::tr("Permanent deletion failed"),
			failures.size() == 1 ? QObject::tr("The item could not be permanently deleted:") : QObject::tr("These items could not be permanently deleted:"),
			failures.join("\n\n"), QMessageBox::Critical);
	}
	return removed;
}

bool PathDeletion::removePathTrashFirstInteractive(const QString& path, QWidget* dialogParent)
{
	return !removePathsInteractive({ path }, Mode::Trash, dialogParent).empty();
}
