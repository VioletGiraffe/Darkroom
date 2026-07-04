#include "Core/IoThreadPool.h"

#include <QThreadPool>

namespace {
	QThreadPool& pool()
	{
		// Leaked on purpose: a function-local QThreadPool would run its destructor (waitForDone + thread join) at
		// static teardown, after QApplication is already gone - a needless hazard. The OS reclaims it on exit.
		static QThreadPool* instance = [] {
			auto* p = new QThreadPool;
			p->setMaxThreadCount(1);        // the whole point: exactly one I/O worker for the entire process
			p->setObjectName("IoThreadPool"); // names the worker thread for the debugger
			return p;
		}();
		return *instance;
	}
}

void IoThreadPool::start(QRunnable* task)
{
	pool().start(task);
}

bool IoThreadPool::tryTake(QRunnable* task)
{
	return pool().tryTake(task);
}
