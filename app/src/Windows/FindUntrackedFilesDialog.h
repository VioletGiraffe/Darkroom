#pragma once

#include <QDialog>
#include <QStringList>

class QListWidget;
class QPushButton;

// ============================================================================
// FindUntrackedFilesDialog - lets the user pick a folder, scans it recursively
// for supported video files that aren't "tracked" (i.e. not recorded as the
// source of any collection's frame folder), and lists the untracked ones.
// Double-clicking a row previews it in the built-in player; the user can
// multi-select rows and send them to the Import staging area via "Send
// selected to staging", which closes this dialog.
//
// All of the scan/folder-picking logic lives here, behind the static scanAndShowUi()
// entry point - callers (MainWindow) just get back the files to stage.
// ============================================================================

class FindUntrackedFilesDialog final : public QDialog
{
public:
	// Picks a folder to scan (persisting the choice for next time), scans it for
	// untracked videos, and shows this dialog if any were found. Returns the
	// files the user chose to send to staging, or an empty list if the user
	// cancelled the folder picker, nothing untracked was found, or the dialog
	// was closed without sending anything.
	[[nodiscard]] static QStringList scanAndShowUi(const QString& rootFolder, QWidget* parent);

private:
	FindUntrackedFilesDialog(const QStringList& untrackedFiles, size_t trackedCount, QWidget* parent);
	~FindUntrackedFilesDialog() override;

	// The files the user chose to send to staging. Non-empty only after the
	// dialog was accepted via "Send selected to staging".
	[[nodiscard]] QStringList selectedForStaging() const { return m_selectedForStaging; }

private:
	QListWidget* m_list = nullptr;
	QPushButton* m_sendButton = nullptr;
	QStringList m_selectedForStaging;
};
