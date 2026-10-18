#include "UiComponents/SortControl.h"
#include "UiComponents/SegmentedToggle.h"
#include "Theme/Theme.h"
#include "compiler/compiler_warnings_control.h"
#include "theme/ctintedsvgiconengine.h"

DISABLE_COMPILER_WARNINGS
#include <QCheckBox>
#include <QCloseEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <functional>

namespace {

const QString SORT_BY_KEY         = "mainWindow/sortBy";
const QString SORT_DESCENDING_KEY = "mainWindow/sortDescending";
const QString FAVORITES_FIRST_KEY = "mainWindow/favoritesFirst";

constexpr int PopoverReopenGuardMs = 100;

class SortPopover final : public QWidget
{
public:
	SortPopover(QWidget* parent, int sortBy, bool descending, bool favoritesFirst,
	            std::function<void(int sortBy, bool descending, bool favoritesFirst)> onChanged,
	            std::function<void()> onClosed)
		: QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), _onChanged(std::move(onChanged)), _onClosed(std::move(onClosed))
	{
		setAttribute(Qt::WA_DeleteOnClose);
		setAttribute(Qt::WA_TranslucentBackground);

		const Theme::Theme& t = Theme::current();

		auto* card = new QFrame(this);
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
		_field = new SegmentedToggle({ tr("Name"), tr("Date") }, card);
		_field->setCurrentIndex(sortBy);
		_field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		connect(_field, &SegmentedToggle::currentChanged, this, [this] { notify(); });
		col->addWidget(_field);

		col->addSpacing(3);
		col->addWidget(makeSectionLabel(tr("Direction"), t));
		_direction = new SegmentedToggle({ tr("↑ Ascending"), tr("↓ Descending") }, card);
		_direction->setCurrentIndex(descending ? 1 : 0);
		_direction->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		connect(_direction, &SegmentedToggle::currentChanged, this, [this] { notify(); });
		col->addWidget(_direction);

		auto* divider = new QFrame(card);
		divider->setFrameShape(QFrame::HLine);
		divider->setFixedHeight(1);
		divider->setStyleSheet(QStringLiteral("background-color: %1; border: none;").arg(t.palette.borderSubtle.name()));
		col->addSpacing(4);
		col->addWidget(divider);
		col->addSpacing(2);

		_favorites = new QCheckBox(tr("Favorites first"), card);
		_favorites->setChecked(favoritesFirst);
		_favorites->setCursor(Qt::PointingHandCursor);
		connect(_favorites, &QCheckBox::toggled, this, [this] { notify(); });
		col->addWidget(_favorites);
	}

	void popupBelow(QWidget* anchor)
	{
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
	void closeEvent(QCloseEvent* e) override
	{
		if (_onClosed)
			_onClosed();
		QWidget::closeEvent(e);
	}

private:
	static constexpr int MARGIN = 22;
	static constexpr int GAP    = 3;
	// Transparent popup area must not overlap the anchor or it intercepts the dismissing click.
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
			_onChanged(_field->currentIndex(), _direction->currentIndex() == 1, _favorites->isChecked());
	}

	std::function<void(int, bool, bool)> _onChanged;
	std::function<void()> _onClosed;
	SegmentedToggle* _field     = nullptr;
	SegmentedToggle* _direction = nullptr;
	QCheckBox*       _favorites = nullptr;
};

} // namespace

SortControl::SortControl(QWidget* parent) : QPushButton(parent)
{
	_sortBy         = QSettings{}.value(SORT_BY_KEY, SortBy::Date).toInt();
	_descending     = QSettings{}.value(SORT_DESCENDING_KEY, true).toBool();
	_favoritesFirst = QSettings{}.value(FAVORITES_FIRST_KEY, false).toBool();

	setToolTip(tr("Sort order"));
	setIcon(tintedSvgIcon(QStringLiteral(":/UI/icon_sort.svg"), [] { return Theme::current().instructionText; }));
	setIconSize(QSize(20, 15));
	updateFace();
	connect(this, &QPushButton::clicked, this, &SortControl::openPopover);
}

void SortControl::updateFace()
{
	setText(QStringLiteral("%1  %2").arg(_sortBy == SortBy::Date ? tr("Date") : tr("Name"), _descending ? "↓" : "↑"));
}

void SortControl::openPopover()
{
	// Qt replays the outside click that closes a popup; do not let it reopen the popup.
	if (_sincePopoverClosed.isValid() && _sincePopoverClosed.elapsed() < PopoverReopenGuardMs)
		return;

	auto* pop = new SortPopover(this, _sortBy, _descending, _favoritesFirst,
		[this](int sortBy, bool descending, bool favoritesFirst) {
			_sortBy         = sortBy;
			_descending     = descending;
			_favoritesFirst = favoritesFirst;
			QSettings s;
			s.setValue(SORT_BY_KEY, _sortBy);
			s.setValue(SORT_DESCENDING_KEY, _descending);
			s.setValue(FAVORITES_FIRST_KEY, _favoritesFirst);
			updateFace();
			emit changed();
		},
		[this] {
			_sincePopoverClosed.restart();
		});

	pop->popupBelow(this);
}
