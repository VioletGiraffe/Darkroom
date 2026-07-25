#pragma once

#include <QString>

#include <functional>

class QWidget;

// App-wide Theme-driven stylesheet; custom widgets paint only what QSS cannot express.
namespace Style {

// Installs now and rebuilds on color-scheme changes.
void install();

// Rebuilds a widget-local sheet now and on theme changes, bound to widget's lifetime.
void applyThemedSheet(QWidget* widget, std::function<QString()> makeSheet);

} // namespace Style
