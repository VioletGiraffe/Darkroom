#pragma once

#include "crashhandler/CCrashHandler.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QString>
RESTORE_COMPILER_WARNINGS

// The startup steps both apps share; the destructor stops the thread pools.
// Must be the first object main constructs.
class Application
{
public:
	Application(int& argc, char** argv, const QString& name, const QString& version, const QString& iconPath);
	~Application();

private:
	// Constructed first: runs the steps the members below require to have run.
	struct EarlySetup
	{
		EarlySetup();
	};

	EarlySetup _earlySetup;
	QApplication _app;
	CCrashHandler _crashHandler;
};
