#include "../Quickroom/src/BatchConvert/BatchConvertPlan.h"

#include "3rdparty/catch2/catch.hpp"

#include <QDir>
#include <QFile>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

using namespace BatchConvert;

namespace {

[[nodiscard]] ResizeOptions resizeTo(QSize box, FitMode fit = FitMode::Inside, ScaleDirection direction = ScaleDirection::Any, bool adaptToOrientation = true)
{
	return { .enabled = true, .fit = fit, .box = box, .direction = direction, .adaptToOrientation = adaptToOrientation };
}

// An empty file at path, with its folder
QString createFile(const QString& path)
{
	REQUIRE(QDir{}.mkpath(QFileInfo{ path }.absolutePath()));
	QFile file{ path };
	REQUIRE(file.open(QIODevice::WriteOnly));
	return path;
}

[[nodiscard]] QStringList targetPaths(const Plan& plan)
{
	QStringList paths;
	for (const Target& target : plan.targets)
		paths.push_back(target.targetPath);
	return paths;
}

} // namespace

TEST_CASE("BatchConvert::outputSize: fits the box inside or outside, or keeps the size", "[batchconvert]")
{
	REQUIRE(outputSize({ 6000, 4000 }, {}) == QSize{ 6000, 4000 }); // resize disabled
	REQUIRE(outputSize({ 6000, 4000 }, resizeTo({ 1920, 1080 })) == QSize{ 1620, 1080 });
	REQUIRE(outputSize({ 6000, 4000 }, resizeTo({ 1920, 1080 }, FitMode::Outside)) == QSize{ 1920, 1280 });
	REQUIRE(outputSize({ 3, 3000 }, resizeTo({ 100, 100 })) == QSize{ 1, 100 }); // a side never rounds to 0
}

TEST_CASE("BatchConvert::outputSize: adapting to the orientation swaps the box for an image of the other orientation", "[batchconvert]")
{
	REQUIRE(outputSize({ 4000, 6000 }, resizeTo({ 1920, 1080 })) == QSize{ 1080, 1620 });
	REQUIRE(outputSize({ 4000, 6000 }, resizeTo({ 1920, 1080 }, FitMode::Inside, ScaleDirection::Any, false)) == QSize{ 720, 1080 });
	// A square image or box has no orientation to adapt to
	REQUIRE(outputSize({ 3000, 3000 }, resizeTo({ 1920, 1080 })) == QSize{ 1080, 1080 });
	REQUIRE(outputSize({ 4000, 6000 }, resizeTo({ 1000, 1000 })) == QSize{ 667, 1000 });
}

TEST_CASE("BatchConvert::outputSize: the direction lock", "[batchconvert]")
{
	REQUIRE(outputSize({ 800, 600 }, resizeTo({ 1920, 1080 }, FitMode::Inside, ScaleDirection::OnlyShrink)) == QSize{ 800, 600 });
	REQUIRE(outputSize({ 6000, 4000 }, resizeTo({ 1920, 1080 }, FitMode::Inside, ScaleDirection::OnlyShrink)) == QSize{ 1620, 1080 });
	REQUIRE(outputSize({ 6000, 4000 }, resizeTo({ 1920, 1080 }, FitMode::Inside, ScaleDirection::OnlyEnlarge)) == QSize{ 6000, 4000 });
	REQUIRE(outputSize({ 800, 600 }, resizeTo({ 1920, 1080 }, FitMode::Inside, ScaleDirection::OnlyEnlarge)) == QSize{ 1440, 1080 });
}

TEST_CASE("BatchConvert::outputMimeType: the chosen format, else the original's when writable, else JPEG", "[batchconvert]")
{
	REQUIRE(outputMimeType("/photos/a.png", { .mimeType = "image/jpeg" }) == "image/jpeg");
	REQUIRE(outputMimeType("/photos/a.png", {}) == "image/png");
	REQUIRE(outputMimeType("/photos/a.JPG", {}) == "image/jpeg");
	REQUIRE(outputMimeType("/photos/a.cr2", {}) == "image/jpeg");
	REQUIRE(outputMimeType("/photos/a.gif", {}) == "image/jpeg");
}

TEST_CASE("BatchConvert::planTargets: a subfolder beside each original, with the format's extension", "[batchconvert]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString png = createFile(dir.filePath("one/a.png"));
	const QString jpeg = createFile(dir.filePath("two/b.JPG"));

	const Plan plan = planTargets({ png, jpeg }, { .mimeType = "image/jpeg" }, ClashResolution::Replace);
	REQUIRE(targetPaths(plan) == QStringList{ dir.filePath("one/converted/a.jpg"), dir.filePath("two/converted/b.JPG") });
	REQUIRE(plan.existingCount == 0);
	REQUIRE(plan.targets[0].mimeType == "image/jpeg");
}

TEST_CASE("BatchConvert::planTargets: a name taken by another target or another source gets a counter", "[batchconvert]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString first = createFile(dir.filePath("one/a.png"));
	const QString second = createFile(dir.filePath("two/a.tif"));
	const Options intoOneFolder{ .mimeType = "image/jpeg", .location = OutputLocation::Folder, .folder = dir.filePath("out") };
	REQUIRE(targetPaths(planTargets({ first, second }, intoOneFolder, ClashResolution::Replace))
		== QStringList{ dir.filePath("out/a.jpg"), dir.filePath("out/a (2).jpg") });

	// a.jpg is a source of the batch: a.png must not overwrite it, a.jpg itself may
	const QString png = createFile(dir.filePath("same/a.png"));
	const QString jpeg = createFile(dir.filePath("same/a.jpg"));
	const Plan plan = planTargets({ png, jpeg }, { .mimeType = "image/jpeg", .location = OutputLocation::SourceFolder }, ClashResolution::Replace);
	REQUIRE(targetPaths(plan) == QStringList{ dir.filePath("same/a (2).jpg"), jpeg });
	REQUIRE(plan.existingCount == 1);
	REQUIRE(plan.replacedSourceCount == 1);
	REQUIRE_FALSE(plan.targets[0].replacesSource);
	REQUIRE(plan.targets[1].replacesSource);
}

TEST_CASE("BatchConvert::planTargets: a target that exists is replaced, kept beside, or skipped", "[batchconvert]")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString source = createFile(dir.filePath("b.png"));
	createFile(dir.filePath("converted/B.JPG")); // names compare case-insensitively
	createFile(dir.filePath("converted/b (2).jpg"));
	const Options options{ .mimeType = "image/jpeg" };

	const Plan replace = planTargets({ source }, options, ClashResolution::Replace);
	REQUIRE(replace.existingCount == 1);
	REQUIRE(replace.replacedSourceCount == 0);
	REQUIRE(targetPaths(replace) == QStringList{ dir.filePath("converted/b.jpg") });
	REQUIRE_FALSE(replace.targets[0].skipped);
	REQUIRE_FALSE(replace.targets[0].replacesSource);

	REQUIRE(targetPaths(planTargets({ source }, options, ClashResolution::KeepBoth)) == QStringList{ dir.filePath("converted/b (3).jpg") });

	const Plan skip = planTargets({ source }, options, ClashResolution::Skip);
	REQUIRE(skip.targets[0].skipped);
}
