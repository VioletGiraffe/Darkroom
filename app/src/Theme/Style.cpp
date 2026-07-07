#include "Theme/Style.h"
#include "Theme/Theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QProxyStyle>
#include <QPushButton>
#include <QStyleHints>
#include <QStyleOption>
#include <QWidget>

#include <utility>

namespace {

// The app-wide sheet is assembled from the per-concern sections below, written against named %Token%
// placeholders: colors spelled exactly like their Theme::ThemeColors field (or Theme:: constant), metrics
// like their Theme:: constant. styleSheetString() concatenates the sections and resolves every token in one
// replace pass. Control backgrounds use palette() roles where possible, so they track the active theme
// without inventing new colors - the sheet is about shape, not the final palette.

constexpr char kButtons[] = R"(
	QPushButton {
		border: 1px solid %BorderStrong%;
		border-radius: %ControlRadius%px;
		padding: 4px 12px;
		background: palette(button);
	}
	/* QSS owns the button frame, so the native style's default-button ring is not drawn - mark it ourselves. */
	QPushButton:default { border-color: %AccentBorder%; }
	QPushButton:hover { border-color: %AccentBorder%; }
	QPushButton:pressed, QPushButton:checked { background: %AccentBg%; border-color: %AccentBorder%; }

	/* Transparent affordance for creating a label (sidebar / quick-import). Left-aligned icon+text, per the mockup. */
	QPushButton#addLabelButton {
		background: transparent;
		border: 2px dashed %BorderStrong%;
		color: %TextPrimary%;
		text-align: left;
		padding-left: 6px;
	}
	QPushButton#addLabelButton:hover { border-color: %AccentBorder%; color: %AccentBorder%; }
)";

constexpr char kTextInputs[] = R"(
	QLineEdit, QPlainTextEdit, QTextEdit {
		border: 1px solid %BorderStrong%;
		border-radius: %ControlRadius%px;
		padding: 4px 8px;
		background: palette(base);
		selection-background-color: %SelectionHighlight%;
		selection-color: %SelectedText%;
	}
	/* 2px border on focus, with 1px less padding per side so the border-box stays the same size and the text doesn't shift. */
	QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border: 2px solid %AccentBorder%; padding: 3px 7px; }
)";

constexpr char kComboBoxes[] = R"(
	QComboBox {
		border: 1px solid %BorderStrong%;
		border-radius: %ControlRadius%px;
		padding: 3px 8px;
		background: palette(button);
	}
	/* :focus needs its own rule - the combo's QSS-owned frame gets no style-drawn focus ring (unlike QPushButton),
	   so keyboard focus would otherwise be invisible. Hover just recolors the 1px edge; focus goes stronger with a
	   2px border, dropping 1px of padding per side to hold the border-box constant so the text doesn't shift. */
	QComboBox:hover { border-color: %AccentBorder%; }
	QComboBox:focus { border: 2px solid %AccentBorder%; padding: 2px 7px; }
	QComboBox::drop-down {
		subcontrol-origin: padding;
		subcontrol-position: center right;
		border: none;
		background: transparent;
		width: 18px;
	}
	QComboBox::down-arrow { image: url(:/UI/combobox_down_arrow.svg); width: 10px; height: 7px; }
	QComboBox QAbstractItemView {
		border: 1px solid %BorderMedium%;
		border-radius: %ControlRadius%px;
		padding: 4px;
		background: palette(base);
		outline: none;
	}
	/* Pin the dropdown row height: styling the view makes QStyleSheetStyle drop the native per-item margins, so
	   without this the rows collapse toward the bare text height - and that default even differs between Qt 6.9
	   and 6.10. Explicit padding keeps rows roomy and identical on every Qt version (vertical echoes QMenu::item,
	   horizontal matches the closed combo field above). */
	QComboBox QAbstractItemView::item { padding: 5px 8px; }
)";

constexpr char kMenus[] = R"(
	QMenu {
		border: 1px solid %BorderMedium%;
		border-radius: %MenuRadius%px;
		padding: 4px;
		background: palette(window);
	}
	QMenu::item { border-radius: %MenuItemRadius%px; padding: 5px 24px 5px 12px; }
	QMenu::item:selected { background: %AccentBg%; color: %AccentText%; }
	QMenu::separator { height: 1px; background: %BorderSubtle%; margin: 4px 8px; }
)";

constexpr char kMenuBar[] = R"(
	/* The bar itself blends into the window; a hovered (:selected) or open (:pressed) item gets the same
	   soft pill treatment as QMenu items. */
	QMenuBar { background: transparent; }
	QMenuBar::item { background: transparent; border-radius: %MenuItemRadius%px; padding: 4px 10px; }
	QMenuBar::item:selected { background: %AccentBg%; color: %AccentText%; }
	QMenuBar::item:pressed { background: %AccentBg%; color: %AccentText%; }
)";

constexpr char kLists[] = R"(
	/* Plain lists get the same hairline border as the other stock controls, but no separate fill - they
	   blend into whatever surface hosts them (sidebar panel, dialog body) rather than standing out as
	   their own "input" surface. Only the list's own frame/background, not item geometry, so it doesn't
	   disturb the grid's sized cards. Per-item coloring (selection, hover) stays local to each list,
	   since that varies by use. */
	QListWidget { border: 1px solid %BorderMedium%; border-radius: %ControlRadius%px; background: transparent; }
)";

constexpr char kScrollBars[] = R"(
	QScrollBar:vertical { border: none; background: transparent; width: %ScrollBarThickness%px; margin: 0; }
	QScrollBar::handle:vertical { background: %MutedText%; border-radius: %ScrollBarHandleRadius%px; min-height: 24px; }
	QScrollBar::handle:vertical:hover { background: %AccentBorder%; }
	QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
	QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

	QScrollBar:horizontal { border: none; background: transparent; height: %ScrollBarThickness%px; margin: 0; }
	QScrollBar::handle:horizontal { background: %MutedText%; border-radius: %ScrollBarHandleRadius%px; min-width: 24px; }
	QScrollBar::handle:horizontal:hover { background: %AccentBorder%; }
	QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
	QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
)";

// Only horizontal rules: the app has no vertical sliders (a future one would fall back to the native look,
// which is the desired "not silently mis-themed" failure mode). NB: a QSS-styled slider does not render tick
// marks at all (QStyleSheetStyle limitation) - PhotoCompareWindow's picker loses its per-photo detent ticks.
constexpr char kSliders[] = R"(
	QSlider::groove:horizontal { border: none; height: %SliderGrooveThickness%px; background: %BorderSubtle%; border-radius: %SliderGrooveRadius%px; }
	QSlider::sub-page:horizontal { background: %AccentBorder%; border-radius: %SliderGrooveRadius%px; }
	QSlider::handle:horizontal {
		width: %SliderHandleContentWidth%px;
		margin: -%SliderHandleOverhang%px 0;   /* let the round handle overhang the thin groove symmetrically */
		background: palette(button);
		border: 1px solid %BorderStrong%;
		border-radius: %SliderHandleRadius%px;
	}
	QSlider::handle:horizontal:hover { border-color: %AccentBorder%; }
	QSlider::handle:horizontal:pressed { background: %AccentBg%; border-color: %AccentBorder%; }
)";

constexpr char kCheckBoxes[] = R"(
	/* An empty rounded outline that fills with the SelectionHighlight blue + a white check mark when checked.
	   width/height size the content box; the 1px border brings the visual box to 16px. It reuses SelectionHighlight
	   (the darkest accent blue) because AccentBorder was too light to carry a white check. Color: checkbox_check.svg. */
	QCheckBox { spacing: 8px; }
	QCheckBox::indicator {
		width: 14px; height: 14px;
		border: 1px solid %BorderStrong%;
		border-radius: %CheckboxRadius%px;
		background: transparent;
	}
	QCheckBox::indicator:hover { border-color: %AccentBorder%; }
	QCheckBox::indicator:checked {
		background: %SelectionHighlight%;
		border-color: %SelectionHighlight%;
		image: url(:/UI/checkbox_check.svg);
	}
)";

constexpr char kGroupBoxes[] = R"(
	/* Hairline card with the title straddling the top border; the title's window-colored background patches
	   the border line out behind the text. */
	QGroupBox {
		border: 1px solid %BorderSubtle%;
		border-radius: %ControlRadius%px;
		margin-top: 1ex;
		padding-top: 2px;
	}
	QGroupBox::title {
		subcontrol-origin: margin;
		subcontrol-position: top left;
		left: 8px;
		padding: 0 3px;
		background-color: palette(window);
		color: %InstructionText%;
	}
)";

constexpr char kSplitters[] = R"(
	/* Invisible until interacted with - the panels' own edges already delimit the split. */
	QSplitter::handle { background: transparent; }
	QSplitter::handle:hover, QSplitter::handle:pressed { background: %AccentBg%; }
)";

constexpr char kToolTips[] = R"(
	/* Square corners on purpose: a tooltip is its own top-level window, so rounding it would need the same
	   hand-painted treatment as the combo popup (ComboPopupRounder) - not worth the machinery here. */
	QToolTip {
		background-color: %BackgroundPrimary%;
		color: %TextPrimary%;
		border: 1px solid %BorderMedium%;
		padding: 4px 8px;
	}
)";

// Grid cards + their thumbnail wells, and the frame-viewer thumbnails, are styled here rather than per
// instance. A per-widget setStyleSheet wraps that widget's subtree in its own QStyleSheetStyle proxy that is
// parsed and resolved on polish, so hundreds of cards/frames each paid that cost on reparent (it dominated
// the grid refresh). Centralized, they share one parsed ruleset matched by object name - and they now follow
// a light/dark switch, which the per-instance sheets (colors baked in at construction) did not.
constexpr char kGridCards[] = R"(
	QWidget#mediaItemCard { background: transparent; border: 1px solid %BorderSubtle%; border-radius: %ControlRadius%px; }
	QWidget#mediaItemCard:hover { background-color: %AccentBg%; border-color: %AccentBorder%; }

	QWidget#cardThumbnailWell { background-color: %ThumbnailMatte%; border: none; border-radius: %ThumbnailMatteRadius%px; font-size: 9pt; }

	QWidget#framedThumbnail { background-color: %ThumbnailMatte%; border: 1px solid %BorderSubtle%; border-radius: %ControlRadius%px; padding: 2px; font-size: 9pt; }
	QWidget#framedThumbnail:hover { background-color: %AccentBg%; border-color: %AccentBorder%; }
)";

constexpr char kStarButtons[] = R"(
	QPushButton#cardStar { border: none; background: transparent; font-size: 11pt; color: %StarButtonDefault%; padding: 0; }
	QPushButton#cardStar:checked { color: %StarActive%; background: transparent; }
	QPushButton#cardStar:hover:!checked { color: %StarButtonHoverUnchecked%; }
	QPushButton#cardStar:checked:hover { color: %StarButtonCheckedHover%; }
)";

QString styleSheetString()
{
	const Theme::ThemeColors& t = Theme::current();

	QString sheet;
	for (const char* section : { kButtons, kTextInputs, kComboBoxes, kMenus, kMenuBar, kLists, kScrollBars,
	                             kSliders, kCheckBoxes, kGroupBoxes, kSplitters, kToolTips, kGridCards, kStarButtons })
		sheet += QLatin1String(section);

	const std::pair<QString, QString> tokens[] = {
		{ QStringLiteral("%BorderSubtle%"),             QString::fromLatin1(t.BorderSubtle) },
		{ QStringLiteral("%BorderMedium%"),             QString::fromLatin1(t.BorderMedium) },
		{ QStringLiteral("%BorderStrong%"),             QString::fromLatin1(t.BorderStrong) },
		{ QStringLiteral("%AccentBorder%"),             QString::fromLatin1(t.AccentBorder) },
		{ QStringLiteral("%AccentBg%"),                 QString::fromLatin1(t.AccentBg) },
		{ QStringLiteral("%AccentText%"),               QString::fromLatin1(t.AccentText) },
		{ QStringLiteral("%SelectionHighlight%"),       QString::fromLatin1(t.SelectionHighlight) },
		{ QStringLiteral("%SelectedText%"),             QString::fromLatin1(t.SelectedText) },
		{ QStringLiteral("%MutedText%"),                QString::fromLatin1(t.MutedText) },
		{ QStringLiteral("%InstructionText%"),          QString::fromLatin1(t.InstructionText) },
		{ QStringLiteral("%BackgroundPrimary%"),        QString::fromLatin1(t.BackgroundPrimary) },
		{ QStringLiteral("%TextPrimary%"),              QString::fromLatin1(t.TextPrimary) },
		{ QStringLiteral("%ThumbnailMatte%"),           QString::fromLatin1(t.ThumbnailMatte) },
		{ QStringLiteral("%StarButtonDefault%"),        QString::fromLatin1(t.StarButtonDefault) },
		{ QStringLiteral("%StarButtonHoverUnchecked%"), QString::fromLatin1(t.StarButtonHoverUnchecked) },
		{ QStringLiteral("%StarButtonCheckedHover%"),   QString::fromLatin1(t.StarButtonCheckedHover) },
		{ QStringLiteral("%StarActive%"),               QString::fromLatin1(Theme::StarActive) },
		{ QStringLiteral("%ControlRadius%"),            QString::number(Theme::ControlRadius) },
		{ QStringLiteral("%MenuRadius%"),               QString::number(Theme::MenuRadius) },
		{ QStringLiteral("%MenuItemRadius%"),           QString::number(Theme::MenuItemRadius) },
		{ QStringLiteral("%CheckboxRadius%"),           QString::number(Theme::CheckboxRadius) },
		{ QStringLiteral("%ThumbnailMatteRadius%"),     QString::number(Theme::ThumbnailMatteRadius) },
		{ QStringLiteral("%ScrollBarThickness%"),       QString::number(Theme::ScrollBarThickness) },
		{ QStringLiteral("%ScrollBarHandleRadius%"),    QString::number(Theme::ScrollBarHandleRadius) },
		{ QStringLiteral("%SliderGrooveThickness%"),    QString::number(Theme::SliderGrooveThickness) },
		{ QStringLiteral("%SliderGrooveRadius%"),       QString::number(Theme::SliderGrooveRadius) },
		{ QStringLiteral("%SliderHandleRadius%"),       QString::number(Theme::SliderHandleRadius) },
		// Derived, not in Theme.h: the handle's QSS content box (border box minus the 1px border per side)
		// and how far the handle sticks out above/below the groove.
		{ QStringLiteral("%SliderHandleContentWidth%"), QString::number(Theme::SliderHandleDiameter - 2) },
		{ QStringLiteral("%SliderHandleOverhang%"),     QString::number((Theme::SliderHandleDiameter - Theme::SliderGrooveThickness) / 2) },
	};
	for (const auto& [token, value] : tokens)
		sheet.replace(token, value);

	return sheet;
}

// The app-wide QPalette matching the current Theme: Window/Base/Button/Text drive every stock control's
// background and text via the palette() QSS roles used above, so they pick up the warm ramps too instead of
// staying on the native OS palette. Highlight/HighlightedText set the palette's selection roles to the same
// SelectionHighlight/SelectedText pair, so genuinely stock (unstyled) palette-driven selection stays consistent
// with the QLineEdit selection styled above. It does NOT reach the QComboBox popup - that view is QSS-styled, so
// QStyleSheetStyle draws its selection and ignores these roles (needs an explicit ::item:selected to change).
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
	p.setColor(QPalette::Highlight, QColor(QString::fromLatin1(t.SelectionHighlight)));
	p.setColor(QPalette::HighlightedText, QColor(QString::fromLatin1(t.SelectedText)));

	// setColor(role, ...) above fills all color groups, including Disabled, with the full-strength colors -
	// without an explicit Disabled override a disabled control's text would not dim at all. Muted matches the
	// sheet's own muted vocabulary (and Mid above, so QSS palette(mid) users read the same tone).
	const QColor muted(QString::fromLatin1(t.MutedText));
	p.setColor(QPalette::PlaceholderText, muted);  // input placeholder hint - the muted tone, not Qt's default half-alpha Text
	p.setColor(QPalette::Disabled, QPalette::WindowText, muted);
	p.setColor(QPalette::Disabled, QPalette::Text, muted);
	p.setColor(QPalette::Disabled, QPalette::ButtonText, muted);
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
			p.setPen(QPen(QColor(QString::fromLatin1(t.BorderMedium)), 1.0));
			p.setBrush(container->palette().base());
			p.drawRoundedRect(r, Theme::ControlRadius, Theme::ControlRadius);
			return true;  // surface drawn; suppress the container's own paint so it doesn't overpaint us (items paint after)
		}
		return QObject::eventFilter(watched, event);
	}
};

// A QSplitterHandle never sets Qt::WA_Hover on itself, so the sheet's QSplitter::handle:hover rule could
// never match: without the attribute the handle neither repaints on enter/leave nor reports State_MouseOver
// (see docs/tips/qt-styling-system-quirks.md). Enable it on each handle as it gets polished - app-wide via
// an event filter because the handles are created internally by QSplitter, leaving no per-instance hook.
class SplitterHandleHoverEnabler : public QObject
{
public:
	using QObject::QObject;

	bool eventFilter(QObject* watched, QEvent* event) override
	{
		if (event->type() == QEvent::Polish && watched->isWidgetType() && watched->inherits("QSplitterHandle"))
			static_cast<QWidget*>(watched)->setAttribute(Qt::WA_Hover);
		return QObject::eventFilter(watched, event);
	}
};

// The keyboard-focus frame on a QPushButton is the base style's PE_FrameFocusRect, drawn around the button's
// label sub-rect - so it hugs the text. QSS doesn't own it (there's no QPushButton:focus rule), so
// QStyleSheetStyle delegates it down to the base style, which is what lets this proxy widen it. Only that one
// primitive is intercepted; every other call passes straight through QProxyStyle to the base style unchanged.
class FocusFrameStyle : public QProxyStyle
{
public:
	using QProxyStyle::QProxyStyle;   // default-constructs with the platform default as the base style

	void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override
	{
		if (element == PE_FrameFocusRect && qobject_cast<const QPushButton*>(widget))
		{
			// qstyleoption_cast, not a sliced copy, so QStyleOptionFocusRect::backgroundColor survives the widen.
			if (const auto* focusOption = qstyleoption_cast<const QStyleOptionFocusRect*>(option))
			{
				QStyleOptionFocusRect widened(*focusOption);
				const int pad = Theme::FocusRectOutset;
				// Grow outward from the label-hugging default, clamped inside the button's own 1px border so the
				// frame nears the edge without sitting on top of it.
				widened.rect = focusOption->rect.adjusted(-pad, -pad, pad, pad).intersected(widget->rect().adjusted(1, 1, -1, -1));
				QProxyStyle::drawPrimitive(element, &widened, painter, widget);
				return;
			}
		}
		QProxyStyle::drawPrimitive(element, option, painter, widget);
	}
};

} // namespace

namespace Style {

void install()
{
	// App-wide base style, wrapped by the stylesheet set just below: QStyleSheetStyle delegates whatever it
	// doesn't own - here the push-button focus frame - down to this proxy. Set before the sheet so the wrap
	// picks it up; deliberately not re-set on colorSchemeChanged, where re-applying the sheet re-wraps this
	// same persistent proxy.
	qApp->setStyle(new FocusFrameStyle);
	qApp->setPalette(paletteFor(Theme::current()));
	qApp->setStyleSheet(styleSheetString());

	// Round the combo drop-down popups (see ComboPopupRounder); parented to qApp for the app's lifetime.
	qApp->installEventFilter(new ComboPopupRounder(qApp));
	// Make the sheet's QSplitter::handle:hover rule reachable (see SplitterHandleHoverEnabler).
	qApp->installEventFilter(new SplitterHandleHoverEnabler(qApp));

	// Re-apply on a light/dark switch (Settings writes the scheme via QStyleHints::setColorScheme, which
	// emits this), so the globally-styled chrome re-derives from the new Theme palette without a restart.
	QObject::connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, qApp, [] {
		qApp->setPalette(paletteFor(Theme::current()));
		qApp->setStyleSheet(styleSheetString());
	});
}

void applyThemedSheet(QWidget* widget, std::function<QString()> makeSheet)
{
	widget->setStyleSheet(makeSheet());
	// Bound to the widget's lifetime, so it auto-disconnects when the widget dies; qApp's styleHints() lives
	// for the whole app. Re-invokes makeSheet each time so it reads the freshly-switched Theme.
	QObject::connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, widget,
	                 [widget, makeSheet = std::move(makeSheet)] { widget->setStyleSheet(makeSheet()); });
}

} // namespace Style
