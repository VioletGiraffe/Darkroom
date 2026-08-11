#include "Windows/LabelManagement.h"
#include "Core/Catalog.h"
#include "Windows/MediaItemManagement.h"

#include "utils/naturalsorting/cnaturalsorterqcollator.h"

#include <QColor>
#include <QColorDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>

#include <algorithm>
#include <vector>

LabelId LabelManagement::createLabelOrReport(
	Catalog& catalog, const QString& name, const QString& color, QWidget* dialogParent)
{
	QString error;
	const LabelId labelId = catalog.createLabel(name, color, &error);
	if (labelId == LabelId::None)
		QMessageBox::warning(dialogParent, QObject::tr("Create label"), error);
	return labelId;
}

LabelId LabelManagement::createLabelInteractive(Catalog& catalog, QWidget* dialogParent)
{
	bool accepted = false;
	const QString name = QInputDialog::getText(
		dialogParent, QObject::tr("New label"), QObject::tr("Label name:"), QLineEdit::Normal, QString{}, &accepted);
	if (!accepted)
		return {};
	return createLabelOrReport(catalog, name, {}, dialogParent);
}

void LabelManagement::renameLabelInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent)
{
	const Catalog::Label* label = catalog.labelById(labelId);
	if (!label)
		return;
	const QString currentName = label->displayName;

	bool accepted = false;
	const QString newName = QInputDialog::getText(
		dialogParent, QObject::tr("Rename label"), QObject::tr("New name:"), QLineEdit::Normal, currentName, &accepted);
	if (!accepted || newName == currentName)
		return;

	Catalog::ChangeBatchScope catalogChanges(catalog);
	QString error;
	if (!catalog.renameLabel(labelId, newName, &error))
		QMessageBox::warning(dialogParent, QObject::tr("Rename label"), error);
}

void LabelManagement::setLabelColorInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent)
{
	const Catalog::Label* label = catalog.labelById(labelId);
	if (!label)
		return;

	const QColor initial = label->color.isEmpty() ? QColor(Qt::white) : QColor(label->color);
	const QColor chosen = QColorDialog::getColor(initial, dialogParent, QObject::tr("Label color"));
	if (!chosen.isValid())
		return;

	Catalog::ChangeBatchScope catalogChanges(catalog);
	catalog.setColor(labelId, chosen.name());
}

void LabelManagement::deleteLabelInteractive(Catalog& catalog, LabelId labelId, QWidget* dialogParent)
{
	const Catalog::Label* label = catalog.labelById(labelId);
	if (!label)
		return;
	const QString name = label->displayName;

	const Catalog::DeleteImpact impact = catalog.deleteLabelImpact(labelId);
	if (impact.wouldOrphan)
	{
		QMessageBox::warning(dialogParent, QObject::tr("Delete label"),
			QObject::tr("Cannot delete \"%1\": some items are stored only under this label, with no other label to "
				"fall back on. Give those items another label first, then delete this one.").arg(name));
		return;
	}

	QString message = QObject::tr("Delete the label \"%1\"?").arg(name);
	if (impact.relocateCount > 0)
		message += QObject::tr("\n\n%1 item(s) stored under it will be moved to another of their labels.").arg(impact.relocateCount);
	if (impact.untagCount > 0)
		message += QObject::tr("\n%1 item(s) tagged with it will lose the tag.").arg(impact.untagCount);
	message += QObject::tr("\n\nThis cannot be undone. Continue?");

	if (QMessageBox::warning(dialogParent, QObject::tr("Delete label"), message,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	Catalog::ChangeBatchScope catalogChanges(catalog);
	std::vector<MediaId> itemsLeftBehind;
	if (!catalog.deleteLabel(labelId, &itemsLeftBehind))
	{
		QString failure = QObject::tr("Could not fully delete \"%1\".").arg(name);
		if (itemsLeftBehind.empty())
			failure += "\n\n" + QObject::tr("Some items may not have been moved.");
		else
		{
			std::ranges::sort(itemsLeftBehind, [&catalog](const MediaId& a, const MediaId& b) {
				return NaturalSort::lessCaseInsensitive(
					MediaItemManagement::itemDisplayName(catalog, a), MediaItemManagement::itemDisplayName(catalog, b));
			});
			failure += "\n\n" + QObject::tr("These items could not be moved and are still stored under it:")
				+ MediaItemManagement::bulletedItemNameList(catalog, itemsLeftBehind);
		}
		QMessageBox::warning(dialogParent, QObject::tr("Delete label"), failure);
	}
}
