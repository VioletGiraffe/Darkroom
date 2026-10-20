#include "UiComponents/ImageSource.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMetaObject>
#include <QObject>
RESTORE_COMPILER_WARNINGS

#include <utility>

namespace ImageSource {

Delivery::Delivery(QObject* context, std::function<void(Result)> done)
	: _context{ context }, _done{ std::move(done) }
{
}

bool Delivery::isCancelled() const
{
	std::lock_guard lock{ _mutex };
	return _context == nullptr;
}

void Delivery::deliver(Result result, std::shared_ptr<const void> owner)
{
	// cancel() shares this lock, so the context outlives the posting.
	std::lock_guard lock{ _mutex };
	if (!_context)
		return;

	// The check repeats on arrival: a cancel may come after the posting. ~QObject discards calls still queued to the context.
	QMetaObject::invokeMethod(_context, [this, owner = std::move(owner), result = std::move(result)]() mutable {
		if (!isCancelled())
			_done(std::move(result));
	}, Qt::QueuedConnection);
}

void Delivery::cancel()
{
	std::lock_guard lock{ _mutex };
	_context = nullptr;
}

Request::Request(std::shared_ptr<Delivery> delivery)
	: _delivery{ std::move(delivery) }
{
}

Request& Request::operator=(Request&& other) noexcept
{
	if (this != &other)
	{
		cancel();
		_delivery = std::move(other._delivery);
	}
	return *this;
}

Request::~Request()
{
	cancel();
}

void Request::cancel()
{
	if (_delivery)
		_delivery->cancel();
	_delivery.reset();
}

} // namespace ImageSource
