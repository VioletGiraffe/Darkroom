#pragma once

#include "Core/LabelId.h"

#include <QString>

class Catalog;
class QWidget;

// Interactive label-editing workflows. Catalog owns validation and filesystem mutations; this module owns dialogs.
namespace LabelManagement
{
	// Reports failure and returns None; an existing same-name label is returned as a success.
	[[nodiscard]] LabelId createLabelOrReport(
		Catalog& catalog, const QString& name, const QString& color, QWidget* dialogParent);

	[[nodiscard]] LabelId createLabelInteractive(Catalog& catalog, QWidget* dialogParent);

	// True means the caller should refresh its catalog view. A partially failed deletion also returns true because
	// Catalog may already have relocated some items before reporting the incomplete operation.
	[[nodiscard]] bool renameLabelInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent);
	[[nodiscard]] bool setLabelColorInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent);
	[[nodiscard]] bool deleteLabelInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent);
}
