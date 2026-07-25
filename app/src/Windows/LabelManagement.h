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

	void renameLabelInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent);
	void setLabelColorInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent);
	void deleteLabelInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent);
}
