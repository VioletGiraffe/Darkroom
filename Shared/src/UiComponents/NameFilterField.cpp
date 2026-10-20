#include "UiComponents/NameFilterField.h"
#include "Theme/Theme.h"
#include "theme/ctintedsvgiconengine.h"

NameFilterField::NameFilterField(QWidget* parent)
	: QLineEdit(parent)
{
	setPlaceholderText(tr("filter by name..."));
	setClearButtonEnabled(true);
	setMinimumWidth(220);
	addAction(tintedSvgIcon(QStringLiteral(":/UI/icon_search.svg"), [] { return Theme::current().palette.textDim; }), QLineEdit::LeadingPosition);
	// Connected first, so owners' textChanged slots match against the new text.
	connect(this, &QLineEdit::textChanged, this, [this](const QString& text) { _query = text.trimmed(); });
}

bool NameFilterField::matches(const QString& name) const
{
	if (_query.isEmpty())
		return true;
	if (_query.startsWith('^'))
		return name.startsWith(_query.mid(1), Qt::CaseInsensitive);
	return name.contains(_query, Qt::CaseInsensitive);
}
