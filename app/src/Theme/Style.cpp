#include "Theme/Style.h"
#include "Theme/Theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QStyleHints>
#include <QWidget>

namespace {

QString styleSheetString()
{
	const Theme::ThemeColors& t = Theme::current();

	// %1 hairline border, %2 accent (hover/focus), %3 soft highlight fill, %4 muted (scrollbar handle).
	// Control backgrounds use palette() roles so they track the active theme without inventing new colors -
	// this sheet is about shape, not the final palette.
	QString sheet = QStringLiteral(R"(
		QPushButton {
			border: 1px solid %1;
			border-radius: %5px;
			padding: 4px 12px;
			background: palette(button);
		}
		QPushButton:hover { border-color: %2; }
		QPushButton:pressed, QPushButton:checked { background: %3; border-color: %2; }
		QPushButton:disabled { color: palette(mid); }

		/* Transparent affordance for adding a label (sidebar). */
		QPushButton#addLabelButton {
			background: transparent;
			border: 2px dashed %1;
			color: %4;
		}
		QPushButton#addLabelButton:hover { border-color: %2; color: %2; }

		QLineEdit, QPlainTextEdit, QTextEdit {
			border: 1px solid %1;
			border-radius: %5px;
			padding: 4px 8px;
			background: palette(base);
			selection-background-color: %2;
		}
		QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border-color: %2; }

		QComboBox {
			border: 1px solid %1;
			border-radius: %5px;
			padding: 3px 8px;
			background: palette(button);
		}
		QComboBox:hover { border-color: %2; }
		QComboBox::drop-down {
			subcontrol-origin: padding;
			subcontrol-position: center right;
			border: none;
			background: transparent;
			width: 18px;
		}
		QComboBox::down-arrow { image: url(:/UI/combobox_down_arrow.svg); width: 10px; height: 7px; }
		QComboBox QAbstractItemView {
			border: 1px solid %1;
			border-radius: %5px;
			padding: 4px;
			background: palette(base);
			outline: none;
		}

		QMenu {
			border: 1px solid %1;
			border-radius: %6px;
			padding: 4px;
			background: palette(window);
		}
		QMenu::item { border-radius: %7px; padding: 5px 24px 5px 12px; }
		QMenu::item:selected { background: %3; }
		QMenu::separator { height: 1px; background: %1; margin: 4px 8px; }

		/* Plain lists get the same hairline border as the other stock controls, but no separate fill - they
		   blend into whatever surface hosts them (sidebar panel, dialog body) rather than standing out as
		   their own "input" surface. Only the list's own frame/background, not item geometry, so it doesn't
		   disturb the grid's sized cards. Per-item coloring (selection, hover) stays local to each list,
		   since that varies by use. */
		QListWidget { border: 1px solid %1; border-radius: %5px; background: transparent; }

		QScrollBar:vertical { border: none; background: transparent; width: %8px; margin: 0; }
		QScrollBar::handle:vertical { background: %4; border-radius: %9px; min-height: 24px; }
		QScrollBar::handle:vertical:hover { background: %2; }
		QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
		QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

		QScrollBar:horizontal { border: none; background: transparent; height: %8px; margin: 0; }
		QScrollBar::handle:horizontal { background: %4; border-radius: %9px; min-width: 24px; }
		QScrollBar::handle:horizontal:hover { background: %2; }
		QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
		QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
	)").arg(t.BorderControl, t.AccentBorder, t.AccentBg, t.MutedText).arg(Theme::ControlRadius)
	   .arg(Theme::MenuRadius).arg(Theme::MenuItemRadius).arg(Theme::ScrollBarThickness).arg(Theme::ScrollBarHandleRadius);

	// Grid cards + their thumbnail wells, and the frame-viewer thumbnails, are styled here rather than per
	// instance. A per-widget setStyleSheet wraps that widget's subtree in its own QStyleSheetStyle proxy that is
	// parsed and resolved on polish, so hundreds of cards/frames each paid that cost on reparent (it dominated
	// the grid refresh). Centralized, they share one parsed ruleset matched by object name - and they now follow
	// a light/dark switch, which the per-instance sheets (colors baked in at construction) did not.
	sheet += QStringLiteral(R"(
		QWidget#mediaItemCard { background: transparent; border: 1px solid %1; border-radius: %5px; }
		QWidget#mediaItemCard:hover { background-color: %3; border-color: %2; }

		QWidget#cardThumbnailWell { background-color: %4; border: none; border-radius: %6px; font-size: 9pt; }

		QWidget#framedThumbnail { background-color: %4; border: 1px solid %1; border-radius: %5px; padding: 2px; font-size: 9pt; }
		QWidget#framedThumbnail:hover { background-color: %3; border-color: %2; }
	)").arg(t.BorderSubtle, t.AccentBorder, t.AccentBg, t.ThumbnailMatte).arg(Theme::ControlRadius).arg(Theme::ThumbnailMatteRadius);

	sheet += QStringLiteral(R"(
		QPushButton#cardStar { border: none; background: transparent; font-size: 11pt; color: %1; padding: 0; }
		QPushButton#cardStar:checked { color: %2; background: transparent; }
		QPushButton#cardStar:hover:!checked { color: %3; }
		QPushButton#cardStar:checked:hover { color: %4; }
	)").arg(t.StarButtonDefault, Theme::StarActive, t.StarButtonHoverUnchecked, t.StarButtonCheckedHover);

	return sheet;
}

// The app-wide QPalette matching the current Theme: Window/Base/Button/Text drive every stock control's
// background and text via the palette() QSS roles used above, so they pick up the warm ramps too instead of
// staying on the native OS palette. Highlight/HighlightedText cover the one native selection state the sheet
// above doesn't reach - the combo drop-down's item selection - so it reads as the same "accent surface" as the
// rest of the app instead of the system's default blue.
QPalette paletteFor(const Theme::ThemeColors& t)
{
	QPalette p;
	const QColor background(QString::fromLatin1(t.BackgroundPrimary));
	const QColor text(QString::fromLatin1(t.TextPrimary));
	p.setColor(QPalette::Window, background);
	p.setColor(QPalette::WindowText, text);
	p.setColor(QPalette::Base, background);
	p.setColor(QPalette::Button, background);
	p.setColor(QPalette::ButtonText, text);
	p.setColor(QPalette::Text, text);
	p.setColor(QPalette::Mid, QColor(QString::fromLatin1(t.MutedText)));
	p.setColor(QPalette::Highlight, QColor(QString::fromLatin1(t.AccentBg)));
	p.setColor(QPalette::HighlightedText, QColor(QString::fromLatin1(t.AccentText)));
	return p;
}

// A combo's drop-down popup is a top-level container (QComboBoxPrivateContainer, a QFrame) hosting the list
// view. Rounding it via QSS is a dead end: as a WA_TranslucentBackground top-level the container won't paint a
// QSS background (it stays invisible), and the inner item view is a scroll area whose opaque viewport paints a
// square over any border-radius (the "flat rectangle"). So we paint the surface directly instead - an
// anti-aliased rounded rect (palette base fill + hairline border) drawn on the container during its paint
// event, with the view and its viewport made transparent so only the items sit on top. App-wide via an event
// filter because the container is private and lazily created on the first popup.
//
// The filter is global (on qApp), so keep the per-event cost trivial: bail on anything but Show/Paint, and on
// Paint require a top-level window before the inherits() check, since the flood of child-widget repaints can
// never be the popup container.
class ComboPopupRounder : public QObject
{
public:
	using QObject::QObject;

	bool eventFilter(QObject* watched, QEvent* event) override
	{
		const QEvent::Type type = event->type();
		if (!watched->isWidgetType() || (type != QEvent::Show && type != QEvent::Paint))
			return QObject::eventFilter(watched, event);

		QWidget* container = static_cast<QWidget*>(watched);
		if (type == QEvent::Show && container->inherits("QComboBoxPrivateContainer"))
		{
			container->setAttribute(Qt::WA_TranslucentBackground);
			if (QAbstractItemView* view = container->findChild<QAbstractItemView*>())
			{
				view->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
				view->viewport()->setAutoFillBackground(false);
			}
		}
		else if (type == QEvent::Paint && container->isWindow() && container->inherits("QComboBoxPrivateContainer"))
		{
			const Theme::ThemeColors& t = Theme::current();
			QPainter p(container);
			p.setRenderHint(QPainter::Antialiasing);
			const QRectF r = QRectF(container->rect()).adjusted(0.5, 0.5, -0.5, -0.5);  // inset so the 1px stroke isn't clipped
			p.setPen(QPen(QColor(QString::fromLatin1(t.BorderControl)), 1.0));
			p.setBrush(container->palette().base());
			p.drawRoundedRect(r, Theme::ControlRadius, Theme::ControlRadius);
			return true;  // surface drawn; suppress the container's own paint so it doesn't overpaint us (items paint after)
		}
		return QObject::eventFilter(watched, event);
	}
};

} // namespace

namespace Style {

void install()
{
	qApp->setPalette(paletteFor(Theme::current()));
	qApp->setStyleSheet(styleSheetString());

	// Round the combo drop-down popups (see ComboPopupRounder); parented to qApp for the app's lifetime.
	qApp->installEventFilter(new ComboPopupRounder(qApp));

	// Re-apply on a light/dark switch (Settings writes the scheme via QStyleHints::setColorScheme, which
	// emits this), so the globally-styled chrome re-derives from the new Theme palette without a restart.
	QObject::connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, qApp, [] {
		qApp->setPalette(paletteFor(Theme::current()));
		qApp->setStyleSheet(styleSheetString());
	});
}

} // namespace Style
