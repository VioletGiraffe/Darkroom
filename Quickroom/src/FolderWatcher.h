#pragma once

#include "compiler/compiler_warnings_control.h"
#include "directory_watcher.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#include <QString>
#include <QTimer>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <memory>
#include <optional>

#ifdef _WIN32
class QWinEventNotifier;
#else
class QSocketNotifier;
#endif

// Calls onChange when listFolder's result for one folder may have changed:
// - on a notification;
// - on every poll tick, while notifications may miss changes or the folder cannot be watched.
// A stopped watch is replaced on the next tick.
// onChange runs from the event loop and must not destroy this watcher.
class FolderWatcher
{
public:
	FolderWatcher(QString folder, std::function<void()> onChange);
	~FolderWatcher();

private:
	// False when the folder cannot be watched.
	bool startWatch();
	void readNotifications();
	void onPollTick();

#ifdef _WIN32
	using Notifier = QWinEventNotifier;
#else
	using Notifier = QSocketNotifier;
#endif

	const QString _folder;
	const std::function<void()> _onChange;
	std::optional<thin_io::directory_watcher> _watcher;
	// Declared after _watcher: destroyed before the handle it waits on is closed.
	// Disabled once the watch has stopped.
	std::unique_ptr<Notifier> _notifier;
	QTimer _pollTimer;
};
