#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QLineEdit>
#include <QString>
RESTORE_COMPILER_WARNINGS

// Search field filtering a grid by item name; the owner refilters on textChanged.
class NameFilterField final : public QLineEdit
{
public:
	explicit NameFilterField(QWidget* parent = nullptr);

	// Case-insensitive substring match; a leading '^' anchors it to the start of the name. An empty filter matches everything.
	[[nodiscard]] bool matches(const QString& name) const;

private:
	QString _query; // trimmed text
};
