#include "App/TestRun.h"
#include "assert/advanced_assert.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QImageReader>
#include <QList>
#include <QMediaFormat>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int VerdictDelayMs = 3'000;

QTemporaryDir& temporaryDir()
{
	static QTemporaryDir dir{ QDir::tempPath() + QStringLiteral("/darkroom-test-run-XXXXXX") };
	return dir;
}

[[nodiscard]] QStringList missingPlugins()
{
	QStringList missing;
	const QList<QByteArray> imageFormats = QImageReader::supportedImageFormats();
	// The non-raw formats of SUPPORTED_IMAGE_SUFFIXES in Utils.cpp, as QImageReader names them
	for (const char* format : { "jpeg", "png", "tiff", "webp", "gif", "bmp", "svg" })
		if (!imageFormats.contains(format))
			missing.push_back(QStringLiteral("the %1 image format").arg(QLatin1StringView{ format }));
	if (QMediaFormat{}.supportedFileFormats(QMediaFormat::Decode).empty())
		missing.push_back(QStringLiteral("a Qt Multimedia backend"));
	return missing;
}

// Empty when the run passed
[[nodiscard]] QString failureReason()
{
	if (const QStringList missing = missingPlugins(); !missing.empty())
		return QStringLiteral("not deployed: ") + missing.join(QStringLiteral(", "));
	if (const QWidget* modal = QApplication::activeModalWidget())
		return QStringLiteral("%1 \"%2\" is open").arg(QLatin1StringView{ modal->metaObject()->className() }, modal->windowTitle());
	if (std::ranges::none_of(QApplication::topLevelWidgets(), [](const QWidget* window) { return window->isVisible(); }))
		return QStringLiteral("no window is shown");
	return {};
}

} // namespace

bool TestRun::requested(int argc, char** argv)
{
	return argc > 1 && std::strcmp(argv[1], "--test-run") == 0;
}

void TestRun::isolateSettings()
{
	assert_r(temporaryDir().isValid());
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporaryDir().path());
}

QString TestRun::temporaryFolder()
{
	return temporaryDir().path();
}

void TestRun::scheduleVerdict()
{
	// Also fires inside a modal dialog's event loop: a dialog blocking startup fails the run instead of hanging it.
	QTimer::singleShot(VerdictDelayMs, qApp, [] {
		if (const QString reason = failureReason(); !reason.isEmpty())
		{
			reportFailure(reason);
			// Not QApplication::exit: an open dialog's exec() would return into code that picks main()'s exit code itself
			std::_Exit(1);
		}

		std::printf("Test run passed\n");
		std::fflush(stdout);
		QApplication::exit(0);
	});
}

void TestRun::reportFailure(const QString& reason)
{
	std::fprintf(stderr, "Test run failed: %s\n", qUtf8Printable(reason));
	std::fflush(stderr);
}
