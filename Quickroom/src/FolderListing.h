#pragma once

#include "UiComponents/SortControl.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDateTime>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <vector>

enum class EntryKind { Folder, Image, Video };

struct FolderEntry
{
	QString path;
	QString name;
	EntryKind kind = EntryKind::Folder;
	qint64 size = 0;
	QDateTime lastModified;
};

// The browser's sort order persists in this group and starts as name ascending.
inline const QString BROWSER_SORT_SETTINGS_GROUP = QStringLiteral("browser");
inline constexpr SortControl::Options BROWSER_SORT_OPTIONS = SortControl::NameField | SortControl::DateField | SortControl::TypeField;

// Folders and supported media files directly in folder, unsorted, with links followed. Hidden entries, broken links
// and special files are left out. nullopt when folder cannot be listed.
[[nodiscard]] std::optional<std::vector<FolderEntry>> listFolder(const QString& folder);

// Ascending: folders first, then by the sort field, ties in name order. Descending reverses all of it.
// Type is the extension; folders have none.
[[nodiscard]] bool precedesInOrder(const FolderEntry& a, const FolderEntry& b, const SortOrder& order);
