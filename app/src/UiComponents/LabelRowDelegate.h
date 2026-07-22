#pragma once

#include <QColor>
#include <QPixmap>
#include <QStyledItemDelegate>

// Paints a whole label row as one unit: a leading color swatch, the name, a right-aligned count, and - for
// rows in the active filter - a pill filled with a translucent tint of that label's own color plus a
// vertical spine threaded through the swatch, fusing with the spines of adjacent active rows into one
// continuous line (the per-row accent color is exactly what plain QSS can't express, which is why this is
// a delegate). Hovered rows get a dashed outline instead. Two special rows are also handled: a StarRole row
// draws a gold star after its swatch (the Best row), and a DividerRole row draws the separator between the
// pinned All/Best rows and the ordinary labels. Everything is driven by the item data roles below, so any
// label list can install it: LabelSidebar uses the full set, ImportDialog's list only swatch + name.
class LabelRowDelegate final : public QStyledItemDelegate {
public:
	// Qt::UserRole itself is left to the owning view - both owners keep their private row id there
	// (LabelId in LabelSidebar, QString in ImportDialog).
	static constexpr int CountRole       = Qt::UserRole + 1;   // QString: the per-row item count, painted right-aligned
	static constexpr int SwatchColorRole = Qt::UserRole + 2;   // QColor: leading swatch color (invalid == no swatch)
	static constexpr int ActiveRole      = Qt::UserRole + 3;   // bool: row is in the active filter (painted highlighted)
	static constexpr int StarRole        = Qt::UserRole + 4;   // bool: draw a gold star after the swatch (the Best row)
	static constexpr int DividerRole     = Qt::UserRole + 5;   // bool: separator line between the pinned rows and the labels
	static constexpr int AllRole         = Qt::UserRole + 6;   // bool: the All row - a stack icon instead of a swatch

	using QStyledItemDelegate::QStyledItemDelegate;

	// The SwatchColorRole value for a label's stored color string: the color itself, or the theme's neutral
	// for an uncolored label. Shared so every list feeding this delegate gets the same fallback.
	static QColor swatchColor(const QString& labelColor);

	QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
	void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
	static qreal iconDpr(const QStyleOptionViewItem& option);
	const QPixmap& allRowIcon(const QColor& color, qreal dpr) const;

	mutable QPixmap _allIcon;
	mutable QColor  _allIconColor;
	mutable qreal   _allIconDpr = 0.0;
};
