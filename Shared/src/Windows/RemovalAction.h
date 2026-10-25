#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <functional>

// What a window's removal command does to the item it shows, in the host's terms.
struct RemovalAction
{
	QString text; // menu wording
	// Returns whether the item left the host's list.
	std::function<bool(const QString& path)> remove;
};
