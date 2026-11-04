#include "RecentPaths.h"
#include "Utils.h"
#include "assert/advanced_assert.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QSettings>
RESTORE_COMPILER_WARNINGS

namespace {

void removeMatches(QStringList& paths, const QString& path)
{
	const QString key = pathComparisonKey(path);
	paths.removeIf([&key](const QString& entry) { return pathComparisonKey(entry) == key; });
}

} // namespace

QStringList RecentPaths::read(QAnyStringView settingsKey)
{
	return QSettings{}.value(settingsKey).toStringList();
}

void RecentPaths::record(QAnyStringView settingsKey, const QString& path, int maxCount)
{
	assert_and_return_r(maxCount > 0, );

	QSettings settings;
	QStringList paths = settings.value(settingsKey).toStringList();
	removeMatches(paths, path);
	paths.prepend(path);
	if (paths.size() > maxCount)
		paths.resize(maxCount);
	settings.setValue(settingsKey, paths);
}

void RecentPaths::remove(QAnyStringView settingsKey, const QString& path)
{
	QSettings settings;
	QStringList paths = settings.value(settingsKey).toStringList();
	removeMatches(paths, path);
	settings.setValue(settingsKey, paths);
}

QString RecentPaths::menuEntryText(qsizetype index, const QString& path)
{
	QString display = QDir::toNativeSeparators(path);
	display.replace('&', QLatin1String("&&"));

	const qsizetype number = index + 1;
	const QString numberText = number < 10 ? QStringLiteral("&%1").arg(number) : number == 10 ? QStringLiteral("1&0") : QString::number(number);
	return numberText + QStringLiteral("  ") + display;
}
