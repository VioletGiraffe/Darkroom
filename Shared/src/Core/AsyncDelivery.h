#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMetaObject>
#include <QObject>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

// The cancellation state of an AsyncDelivery, which AsyncRequest holds whatever the result type.
class AsyncDeliveryState {
public:
	[[nodiscard]] bool isCancelled() const;
	void cancel();

protected:
	explicit AsyncDeliveryState(QObject* context) noexcept;

	mutable std::mutex _mutex;
	QObject* _context; // null once cancelled
};

// Shared by an asynchronous job's stages, on any thread, and its AsyncRequest.
template <typename Result>
class AsyncDelivery final : public AsyncDeliveryState {
public:
	// done runs on the context's thread but may be destroyed on any thread: its captures must allow that.
	AsyncDelivery(QObject* context, std::function<void(Result)> done)
		: AsyncDeliveryState{ context }, _done{ std::move(done) }
	{
	}

	// Queues the completion on the context's thread unless cancelled; never touches the context after a cancel.
	// owner keeps this delivery alive until the queued call runs.
	void deliver(Result result, std::shared_ptr<const void> owner)
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

private:
	const std::function<void(Result)> _done;
};

// Destroying or reassigning it cancels its job's delivery: the completion never runs afterwards.
class AsyncRequest final {
public:
	AsyncRequest() = default;
	explicit AsyncRequest(std::shared_ptr<AsyncDeliveryState> delivery);
	AsyncRequest(AsyncRequest&& other) noexcept = default;
	AsyncRequest& operator=(AsyncRequest&& other) noexcept;
	~AsyncRequest();

private:
	void cancel();

	std::shared_ptr<AsyncDeliveryState> _delivery;
};
