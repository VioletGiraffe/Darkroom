#pragma once

#include "Core/AsyncDelivery.h"
#include "ImageMetadata/ImageInfo.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <set>

class QLineEdit;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTreeView;

// What an image file states about itself, as name-value rows in collapsible groups under a filter field.
// - Reads the file on IoThreadPool, and only while visible.
// - Every group starts expanded; a group the user collapses stays collapsed for the next files.
// - While the filter has text, only matching rows show, every group expanded; a group whose title matches shows all its rows.
// - Esc clears the filter field when it has text, else moves the focus to focusOnEscape. Ctrl+C in the rows copies the selected ones.
//   The window's shortcuts see neither key.
// - Takes the wheel events its children leave: they never reach the window.
class ImageInfoPanel final : public QWidget
{
public:
	ImageInfoPanel(QWidget* focusOnEscape, QWidget* parent);

	// Read now if the panel is visible, else when it is next shown; the previous file's rows stay until then
	void setPath(const QString& path);
	// For a file changed on disk: reads it again, as setPath() reads a new one
	void rereadFile();
	void focusFilter();

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;

private:
	void readUnlessRequested();
	void showInfo(const QString& path, const ImageMetadata::ImageInfo& info);
	void applyFilter(const QString& text);
	// Every group while the filter has text, else those the user has not collapsed
	void expandGroups();
	// Requires that every top-level row is a group
	void spanGroupRows();
	void showContextMenu(QPoint position);
	// The rows passing the filter, in view order: each group's title, then its rows' names and full values, tab-separated
	[[nodiscard]] QString rowsText(bool selectedOnly) const;

private:
	QWidget* const _focusOnEscape;
	QString _path;
	QString _requestedPath; // the file last read, or being read
	AsyncRequest _read;
	std::set<QString> _collapsedGroups; // by title
	bool _nameColumnSized = false;

	QLineEdit* _filter = nullptr;
	QTreeView* _tree = nullptr;
	QStandardItemModel* _model = nullptr;
	QSortFilterProxyModel* _proxy = nullptr;
};
