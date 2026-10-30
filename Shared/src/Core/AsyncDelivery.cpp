#include "Core/AsyncDelivery.h"

AsyncDeliveryState::AsyncDeliveryState(QObject* context) noexcept
	: _context{ context }
{
}

bool AsyncDeliveryState::isCancelled() const
{
	std::lock_guard lock{ _mutex };
	return _context == nullptr;
}

void AsyncDeliveryState::cancel()
{
	std::lock_guard lock{ _mutex };
	_context = nullptr;
}

AsyncRequest::AsyncRequest(std::shared_ptr<AsyncDeliveryState> delivery)
	: _delivery{ std::move(delivery) }
{
}

AsyncRequest& AsyncRequest::operator=(AsyncRequest&& other) noexcept
{
	if (this != &other)
	{
		cancel();
		_delivery = std::move(other._delivery);
	}
	return *this;
}

AsyncRequest::~AsyncRequest()
{
	cancel();
}

void AsyncRequest::cancel()
{
	if (_delivery)
		_delivery->cancel();
	_delivery.reset();
}
