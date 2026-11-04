#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QAnyStringView>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

// A most-recent-first path list in the app's QSettings store, one list per settings key.
// Paths are matched by pathComparisonKey and never checked on disk: an entry may be on an unavailable drive or network share.
namespace RecentPaths
{
	[[nodiscard]] QStringList read(QAnyStringView settingsKey);
	// Puts path first, dropping its earlier occurrence and the entries beyond maxCount.
	void record(QAnyStringView settingsKey, const QString& path, int maxCount);
	void remove(QAnyStringView settingsKey, const QString& path);

	// The menu text for the path at index in a list: its native spelling, numbered, with the accelerators 1 to 9 and 0 for the first ten.
	[[nodiscard]] QString menuEntryText(qsizetype index, const QString& path);
}
