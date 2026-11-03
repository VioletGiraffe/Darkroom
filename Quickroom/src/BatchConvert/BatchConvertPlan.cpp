#include "BatchConvertPlan.h"
#include "Utils.h"
#include "assert/advanced_assert.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QMimeType>
#include <QSet>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cmath>
#include <utility>

namespace BatchConvert {

namespace {

QString targetFolder(const QFileInfo& source, const Options& options)
{
	switch (options.location)
	{
	case OutputLocation::Subfolder: return QDir{ source.absolutePath() }.filePath(options.subfolderName);
	case OutputLocation::Folder: return options.folder;
	case OutputLocation::SourceFolder: return source.absolutePath();
	}
	assert_unconditional_r("Unknown OutputLocation");
	return source.absolutePath();
}

// Whether a folder holds an entry of a name, from one listing of each folder; case-insensitive everywhere
class FolderNames
{
public:
	[[nodiscard]] bool contains(const QString& folder, const QString& name)
	{
		const QString folderKey = pathComparisonKey(folder);
		auto names = _namesByFolder.find(folderKey);
		if (names == _namesByFolder.end())
		{
			QSet<QString> listed;
			for (const QString& entry : QDir{ folder }.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
				listed.insert(entry.toCaseFolded());
			names = _namesByFolder.insert(folderKey, std::move(listed));
		}
		return names->contains(name.toCaseFolded());
	}

private:
	QHash<QString, QSet<QString>> _namesByFolder;
};

} // namespace

QSize outputSize(QSize uprightSize, const ResizeOptions& resize)
{
	if (!resize.enabled || uprightSize.isEmpty() || resize.box.isEmpty())
		return uprightSize;

	QSize box = resize.box;
	const bool square = uprightSize.width() == uprightSize.height(), squareBox = box.width() == box.height();
	const bool landscape = uprightSize.width() > uprightSize.height(), landscapeBox = box.width() > box.height();
	if (resize.adaptToOrientation && !square && !squareBox && landscape != landscapeBox)
		box.transpose();

	const double widthScale = static_cast<double>(box.width()) / uprightSize.width();
	const double heightScale = static_cast<double>(box.height()) / uprightSize.height();
	double scale = resize.fit == FitMode::Inside ? std::min(widthScale, heightScale) : std::max(widthScale, heightScale);
	if (resize.direction == ScaleDirection::OnlyShrink)
		scale = std::min(scale, 1.0);
	else if (resize.direction == ScaleDirection::OnlyEnlarge)
		scale = std::max(scale, 1.0);

	const auto scaled = [&](int side) { return std::max(1, static_cast<int>(std::lround(side * scale))); };
	return QSize{ scaled(uprightSize.width()), scaled(uprightSize.height()) };
}

QString outputMimeType(const QString& sourcePath, const Options& options)
{
	if (!options.mimeType.isEmpty())
		return options.mimeType;

	// By extension: planning reads no file
	const QString ownType = ImageEncoding::writableMimeTypeOf(QMimeDatabase{}.mimeTypeForFile(sourcePath, QMimeDatabase::MatchExtension));
	return ownType.isEmpty() ? QStringLiteral("image/jpeg") : ownType;
}

Plan planTargets(const QStringList& sourcePaths, const Options& options, ClashResolution resolution)
{
	const QMimeDatabase mimeDatabase;
	QHash<QString, QString> mimeTypeBySuffix; // outputMimeType() goes by extension alone
	FolderNames namesOnDisk;

	QSet<QString> sourceKeys;
	for (const QString& sourcePath : sourcePaths)
		sourceKeys.insert(pathComparisonKey(sourcePath));
	QSet<QString> takenKeys; // the target paths of earlier targets

	Plan plan;
	for (const QString& sourcePath : sourcePaths)
	{
		const QFileInfo source{ sourcePath };
		const QString suffixKey = source.suffix().toLower();
		if (!mimeTypeBySuffix.contains(suffixKey))
			mimeTypeBySuffix.insert(suffixKey, outputMimeType(sourcePath, options));
		const QString mimeType = mimeTypeBySuffix.value(suffixKey);

		// The source's own spelling of the extension when it names the output format, so a source can replace itself
		const QMimeType outputType = mimeDatabase.mimeTypeForName(mimeType);
		const QString suffix = mimeDatabase.mimeTypeForFile(source.fileName(), QMimeDatabase::MatchExtension) == outputType ? source.suffix() : outputType.preferredSuffix();
		const QString folder = targetFolder(source, options);
		const QString baseName = source.completeBaseName();
		const auto nameWithCounter = [&](int counter) {
			return counter < 2 ? QStringLiteral("%1.%2").arg(baseName, suffix) : QStringLiteral("%1 (%2).%3").arg(baseName).arg(counter).arg(suffix);
		};

		const QString ownKey = pathComparisonKey(sourcePath);
		const auto takenByBatch = [&](const QString& name) {
			const QString key = pathComparisonKey(QDir{ folder }.filePath(name));
			return takenKeys.contains(key) || (key != ownKey && sourceKeys.contains(key));
		};

		int counter = 1;
		while (takenByBatch(nameWithCounter(counter)))
			++counter;

		Target target{ .sourcePath = sourcePath, .mimeType = mimeType };
		if (namesOnDisk.contains(folder, nameWithCounter(counter)))
		{
			++plan.existingCount;
			const bool replacesSource = pathComparisonKey(QDir{ folder }.filePath(nameWithCounter(counter))) == ownKey;
			if (replacesSource)
				++plan.replacedSourceCount;

			if (resolution == ClashResolution::KeepBoth)
			{
				do
					++counter;
				while (takenByBatch(nameWithCounter(counter)) || namesOnDisk.contains(folder, nameWithCounter(counter)));
			}
			else
			{
				target.replacesSource = replacesSource;
				target.skipped = resolution == ClashResolution::Skip;
			}
		}

		target.targetPath = QDir{ folder }.filePath(nameWithCounter(counter));
		if (!target.skipped)
			takenKeys.insert(pathComparisonKey(target.targetPath));
		plan.targets.push_back(std::move(target));
	}
	return plan;
}

} // namespace BatchConvert
