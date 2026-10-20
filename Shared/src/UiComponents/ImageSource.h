#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QImage>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <memory>
#include <mutex>

class QObject;

// Types shared by the image sources a ThumbnailWidget shows. Every source has these, called on the GUI thread:
//   std::optional<Result> renderNow(QSize box, qreal dpr) const - the result when it is cheap to produce there; empty otherwise
//   Request renderAsync(QSize box, qreal dpr, QObject* context, std::function<void(Result)> done) const - only called after
//     renderNow returned empty; done runs on context's thread, never after context is destroyed
//   bool roundsCorners() const - whether the shown image's corners are rounded to the matte
// An image fits the requested box: logical size within it, device pixel ratio as requested.
namespace ImageSource {

struct Result {
	QImage image;   // null with an empty error: nothing to show
	QString error;  // non-empty: why there is no image
};

// Shared by an asynchronous render's stages, on any thread, and its Request.
class Delivery final {
public:
	Delivery(QObject* context, std::function<void(Result)> done);

	[[nodiscard]] bool isCancelled() const;
	// Queues the completion on the context's thread unless cancelled; never touches the context after a cancel.
	// owner keeps this delivery alive until the queued call runs.
	void deliver(Result result, std::shared_ptr<const void> owner);
	void cancel();

private:
	mutable std::mutex _mutex;
	QObject* _context; // null once cancelled
	const std::function<void(Result)> _done;
};

// Destroying or reassigning it cancels its render: the completion never runs afterwards.
class Request final {
public:
	Request() = default;
	explicit Request(std::shared_ptr<Delivery> delivery);
	Request(Request&& other) noexcept = default;
	Request& operator=(Request&& other) noexcept;
	~Request();

private:
	void cancel();

	std::shared_ptr<Delivery> _delivery;
};

} // namespace ImageSource
