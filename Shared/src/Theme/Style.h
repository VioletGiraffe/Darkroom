#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <functional>

class QWidget;

// App-wide Theme-driven stylesheet; custom widgets paint only what QSS cannot express.
namespace Style {

// Installs now and rebuilds on color-scheme changes.
void install();

// Rebuilds a widget-local sheet now and on theme changes, bound to widget's lifetime.
void applyThemedSheet(QWidget* widget, std::function<QString()> makeSheet);

} // namespace Style
