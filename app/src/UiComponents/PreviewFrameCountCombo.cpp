#include "UiComponents/PreviewFrameCountCombo.h"
#include "Settings.h"
#include "Theme/Theme.h"
#include "theme/ctintedsvgiconengine.h"

#include <QAbstractItemView>
#include <QIcon>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

namespace {

constexpr int MIN_PREVIEW_FRAME_COUNT = 1;
constexpr int MAX_PREVIEW_FRAME_COUNT = 10;

// QComboBox paints the current item's icon itself; popup rows should remain undecorated.
class ClosedControlIconDelegate final : public QStyledItemDelegate
{
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
	{
		QStyledItemDelegate::initStyleOption(option, index);
		option->features &= ~QStyleOptionViewItem::HasDecoration;
		option->icon = QIcon();
	}
};

} // namespace

PreviewFrameCountCombo::PreviewFrameCountCombo(int initialFrameCount, QWidget* parent)
	: QComboBox(parent)
{
	setToolTip(tr("Number of preview frames shown on each video card"));
	const QIcon previewCountIcon = tintedSvgIcon(QStringLiteral(":/UI/icon_columns.svg"), [] { return Theme::current().instructionText; });
	for (int n = MIN_PREVIEW_FRAME_COUNT; n <= MAX_PREVIEW_FRAME_COUNT; ++n)
		addItem(previewCountIcon, (n == 1 ? tr("%1 frame per preview") : tr("%1 frames per preview")).arg(n), n);
	view()->setItemDelegate(new ClosedControlIconDelegate(this));

	const int initialIndex = findData(initialFrameCount);
	setCurrentIndex(initialIndex >= 0 ? initialIndex : findData(Defaults::PreviewFrameCount));
}

int PreviewFrameCountCombo::frameCount() const
{
	return currentData().toInt();
}
