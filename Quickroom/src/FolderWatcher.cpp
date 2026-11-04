#include "FolderWatcher.h"
#include "qt_paths.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#ifdef _WIN32
#include <QWinEventNotifier>
#else
#include <QSocketNotifier>
#endif
RESTORE_COMPILER_WARNINGS

#include <chrono>
#include <utility>

FolderWatcher::FolderWatcher(QString folder, std::function<void()> onChange) :
	_folder{ std::move(folder) },
	_onChange{ std::move(onChange) }
{
	_pollTimer.setInterval(std::chrono::seconds{ 3 });
	QObject::connect(&_pollTimer, &QTimer::timeout, &_pollTimer, [this] { onPollTick(); });
	if (!startWatch() || _watcher->coverage() == thin_io::watch_coverage::partial)
		_pollTimer.start();
}

FolderWatcher::~FolderWatcher() = default;

bool FolderWatcher::startWatch()
{
	auto watcher = thin_io::directory_watcher::watch(thin_io::native_path(_folder).data(), { .recursive = false });
	if (!watcher)
		return false;

	_watcher = std::move(*watcher);
#ifdef _WIN32
	_notifier = std::make_unique<Notifier>(_watcher->wait_handle());
#else
	_notifier = std::make_unique<Notifier>(_watcher->wait_handle(), QSocketNotifier::Read);
#endif
	QObject::connect(_notifier.get(), &Notifier::activated, _notifier.get(), [this] { readNotifications(); });
	return true;
}

void FolderWatcher::readNotifications()
{
	const auto events = _watcher->read_events();
	if (events && events->empty())
		return;

	// The stopped event comes last.
	if (!events || events->back().kind == thin_io::watch_event_kind::stopped)
	{
		// The notifier must not be destroyed while it emits: the next tick replaces the watch.
		_notifier->setEnabled(false);
		_pollTimer.start();
	}
	_onChange();
}

void FolderWatcher::onPollTick()
{
	if (_notifier && !_notifier->isEnabled())
	{
		_notifier.reset();
		_watcher.reset();
	}
	if (!_watcher && startWatch() && _watcher->coverage() == thin_io::watch_coverage::complete)
		_pollTimer.stop();
	_onChange();
}
