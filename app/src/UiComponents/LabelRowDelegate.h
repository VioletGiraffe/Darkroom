#pragma once

#include <QColor>
#include <QPixmap>
#include <QStyledItemDelegate>

// Paints label rows from the roles below: swatch/name/count, per-label active tint and connecting spine,
// hover outline, Best star, All icon, and divider. A delegate is required because QSS cannot vary by label color.
class LabelRowDelegate final : public QStyledItemDelegate {
public:
	// Qt::UserRole remains available to the owning view for its row id.
	static constexpr int CountRole       = Qt::UserRole + 1;
	static constexpr int SwatchColorRole = Qt::UserRole + 2;
	static constexpr int ActiveRole      = Qt::UserRole + 3;
	static constexpr int StarRole        = Qt::UserRole + 4;
	static constexpr int DividerRole     = Qt::UserRole + 5;
	static constexpr int AllRole         = Qt::UserRole + 6;

	using QStyledItemDelegate::QStyledItemDelegate;

	// Converts an empty stored color to the theme's neutral swatch.
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
