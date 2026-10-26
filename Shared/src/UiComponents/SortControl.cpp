#include "UiComponents/SortControl.h"
#include "UiComponents/SegmentedToggle.h"
#include "Theme/Theme.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "theme/ctintedsvgiconengine.h"

DISABLE_COMPILER_WARNINGS
#include <QCheckBox>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace {

// Appended to the settings group.
const QString SORT_BY_KEY         = "/sortBy";
const QString SORT_DESCENDING_KEY = "/sortDescending";
const QString FAVORITES_FIRST_KEY = "/favoritesFirst";

constexpr std::array<int, 3> ALL_FIELDS{ SortBy::Name, SortBy::Date, SortBy::Type };

bool offersField(SortControl::Options options, int sortBy)
{
	return std::ranges::find(ALL_FIELDS, sortBy) != ALL_FIELDS.end() && options.testFlag(SortControl::Option(1 << sortBy));
}

QString fieldLabel(int sortBy)
{
	switch (sortBy)
	{
	case SortBy::Name: return QObject::tr("Name");
	case SortBy::Date: return QObject::tr("Date");
	case SortBy::Type: return QObject::tr("Type");
	}
	assert_unconditional_r("Unknown sort field");
	return {};
}

class SortPopover final : public QWidget
{
public:
	// NoDropShadowWindowHint: the system shadow outlines the whole window, transparent margin included.
	SortPopover(QWidget* parent, const SortOrder& order, SortControl::Options options, std::function<void(const SortOrder& order)> onChanged)
		: QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), _onChanged(std::move(onChanged))
	{
		setAttribute(Qt::WA_DeleteOnClose);
		setAttribute(Qt::WA_TranslucentBackground);

		const Theme::Theme& t = Theme::current();

		auto* card = _card = new QFrame(this);
		card->setObjectName("sortPopoverCard");
		card->setStyleSheet(QStringLiteral(
			"QFrame#sortPopoverCard { background-color: palette(window); border: 1px solid %1; border-radius: %2px; }")
			.arg(t.palette.border.name()).arg(t.metrics.popoverRadius));

		auto* shadow = new QGraphicsDropShadowEffect(card);
		shadow->setBlurRadius(20);
		shadow->setOffset(0, 5);
		shadow->setColor(QColor(0, 0, 0, 60));
		card->setGraphicsEffect(shadow);

		auto* outer = new QVBoxLayout(this);
		outer->setContentsMargins(MARGIN, TOP_MARGIN, MARGIN, MARGIN);
		outer->addWidget(card);

		auto* col = new QVBoxLayout(card);
		col->setContentsMargins(10, 10, 10, 10);
		col->setSpacing(6);

		col->addWidget(makeSectionLabel(tr("Sort by"), t));
		QStringList fieldLabels;
		for (const int field : ALL_FIELDS)
		{
			if (!offersField(options, field))
				continue;
			_offeredFields.push_back(field);
			fieldLabels.push_back(fieldLabel(field));
		}
		_field = new SegmentedToggle(fieldLabels, card);
		_field->setCurrentIndex(static_cast<int>(std::ranges::find(_offeredFields, order.sortBy) - _offeredFields.begin()));
		_field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		connect(_field, &SegmentedToggle::currentChanged, this, [this] { notify(); });
		col->addWidget(_field);

		col->addSpacing(3);
		col->addWidget(makeSectionLabel(tr("Direction"), t));
		_direction = new SegmentedToggle({ tr("↑ Ascending"), tr("↓ Descending") }, card);
		_direction->setCurrentIndex(order.descending ? 1 : 0);
		_direction->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		connect(_direction, &SegmentedToggle::currentChanged, this, [this] { notify(); });
		col->addWidget(_direction);

		if (!options.testFlag(SortControl::FavoritesOption))
			return;

		auto* divider = new QFrame(card);
		divider->setFrameShape(QFrame::HLine);
		divider->setFixedHeight(1);
		divider->setStyleSheet(QStringLiteral("background-color: %1; border: none;").arg(t.palette.borderSubtle.name()));
		col->addSpacing(4);
		col->addWidget(divider);
		col->addSpacing(2);

		_favorites = new QCheckBox(tr("Favorites first"), card);
		_favorites->setChecked(order.favoritesFirst);
		_favorites->setCursor(Qt::PointingHandCursor);
		connect(_favorites, &QCheckBox::toggled, this, [this] { notify(); });
		col->addWidget(_favorites);
	}

	void popupBelow(QWidget* anchor)
	{
		_anchor = anchor;
		adjustSize();
		const QPoint topLeft = anchor->mapToGlobal(QPoint(0, 0));
		int x = topLeft.x() + anchor->width() - width() + MARGIN;
		int y = topLeft.y() + anchor->height() + GAP - TOP_MARGIN;
		if (QScreen* scr = anchor->screen())
		{
			const QRect avail = scr->availableGeometry();
			x = qBound(avail.left() - MARGIN, x, avail.right() + MARGIN - width());
		}
		move(x, y);
		show();
	}

protected:
	// Receives every press while open, outside ones included: a popup grabs the mouse.
	void mousePressEvent(QMouseEvent* e) override
	{
		e->accept();
		// Qt replays a dismissing press to the widget under it: on the anchor, that would reopen this popup.
		if (_anchor->rect().contains(_anchor->mapFromGlobal(e->globalPosition().toPoint())))
			setAttribute(Qt::WA_NoMouseReplay);
		// The transparent margin counts as outside.
		if (!_card->geometry().contains(e->position().toPoint()))
			close();
	}

private:
	static constexpr int MARGIN = 22;
	static constexpr int GAP    = 3;
	static constexpr int TOP_MARGIN = GAP;

	static QLabel* makeSectionLabel(const QString& text, const Theme::Theme& t)
	{
		auto* label = new QLabel(text);
		QFont f = label->font();
		if (f.pointSizeF() > 0)
			f.setPointSizeF(f.pointSizeF() - 1.0);
		label->setFont(f);
		label->setStyleSheet(QStringLiteral("color: %1;").arg(t.palette.textDim.name()));
		return label;
	}

	void notify()
	{
		if (_onChanged)
			_onChanged({ _offeredFields[static_cast<size_t>(_field->currentIndex())], _direction->currentIndex() == 1, _favorites && _favorites->isChecked() });
	}

	std::function<void(const SortOrder&)> _onChanged;
	std::vector<int> _offeredFields; // in segment order
	QWidget*         _anchor    = nullptr;
	QFrame*          _card      = nullptr;
	SegmentedToggle* _field     = nullptr;
	SegmentedToggle* _direction = nullptr;
	QCheckBox*       _favorites = nullptr; // null without FavoritesOption
};

} // namespace

SortControl::SortControl(const QString& settingsGroup, const SortOrder& defaultOrder, Options options, QWidget* parent)
	: QPushButton(parent)
	, _settingsGroup{ settingsGroup }
	, _options{ options }
	, _order{ savedOrder(settingsGroup, defaultOrder, options) }
{
	setToolTip(tr("Sort order"));
	setIcon(tintedSvgIcon(QStringLiteral(":/UI/icon_sort.svg"), [] { return Theme::current().instructionText; }));
	setIconSize(QSize(20, 15));
	updateFace();
	connect(this, &QPushButton::clicked, this, &SortControl::openPopover);
}

SortOrder SortControl::savedOrder(const QString& settingsGroup, const SortOrder& defaultOrder, Options options)
{
	assert_r(offersField(options, defaultOrder.sortBy));
	const QSettings s;
	const int savedSortBy = s.value(settingsGroup + SORT_BY_KEY, defaultOrder.sortBy).toInt();
	return {
		.sortBy         = offersField(options, savedSortBy) ? savedSortBy : defaultOrder.sortBy,
		.descending     = s.value(settingsGroup + SORT_DESCENDING_KEY, defaultOrder.descending).toBool(),
		.favoritesFirst = options.testFlag(FavoritesOption) && s.value(settingsGroup + FAVORITES_FIRST_KEY, defaultOrder.favoritesFirst).toBool(),
	};
}

void SortControl::updateFace()
{
	setText(QStringLiteral("%1  %2").arg(fieldLabel(_order.sortBy), _order.descending ? "↓" : "↑"));
}

void SortControl::openPopover()
{
	auto* pop = new SortPopover(this, _order, _options,
		[this](const SortOrder& order) {
			_order = order;
			QSettings s;
			s.setValue(_settingsGroup + SORT_BY_KEY, _order.sortBy);
			s.setValue(_settingsGroup + SORT_DESCENDING_KEY, _order.descending);
			if (_options.testFlag(FavoritesOption))
				s.setValue(_settingsGroup + FAVORITES_FIRST_KEY, _order.favoritesFirst);
			updateFace();
			emit changed();
		});

	pop->popupBelow(this);
}
