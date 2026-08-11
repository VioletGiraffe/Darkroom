#include "Theme/Style.h"
#include "Theme/Theme.h"

#include "assert/advanced_assert.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QComboBox>
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
	/* Compensate for the thicker focus border to prevent layout movement. */
	QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border: 2px solid %AccentBorder%; padding: 3px 7px; }
)";

constexpr char kComboBoxes[] = R"(
	QComboBox {
		border: 1px solid %BorderStrong%;
		border-radius: %ControlRadius%px;
		padding: 3px 8px;
		background: palette(button);
	}
	/* QSS-owned combo frames do not receive a native focus ring. */
	QComboBox:hover { border-color: %AccentBorder%; }
	QComboBox:focus { border: 2px solid %AccentBorder%; padding: 2px 7px; }
	QComboBox::drop-down {
		subcontrol-origin: padding;
		subcontrol-position: center right;
		border: none;
		background: transparent;
		width: 18px;
	}
	QComboBox::down-arrow { image: url(%ComboArrowIcon%); width: 10px; height: 7px; }
	QComboBox QAbstractItemView {
		border: 1px solid %BorderMedium%;
		border-radius: %ControlRadius%px;
		padding: 4px;
		background: palette(base);
		outline: none;
	}
	/* QStyleSheetStyle otherwise drops the native per-item margins. */
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
	QMenuBar { background: transparent; }
	QMenuBar::item { background: transparent; border-radius: %MenuItemRadius%px; padding: 4px 10px; }
	QMenuBar::item:selected { background: %AccentBg%; color: %AccentText%; }
	QMenuBar::item:pressed { background: %AccentBg%; color: %AccentText%; }
)";

constexpr char kLists[] = R"(
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

// QStyleSheetStyle does not render slider tick marks.
constexpr char kSliders[] = R"(
	QSlider::groove:horizontal { border: none; height: %SliderGrooveThickness%px; background: %BorderSubtle%; border-radius: %SliderGrooveRadius%px; }
	QSlider::sub-page:horizontal { background: %AccentBorder%; border-radius: %SliderGrooveRadius%px; }
	QSlider::handle:horizontal {
		width: %SliderHandleContentWidth%px;
		margin: -%SliderHandleOverhang%px 0;
		background: palette(button);
		border: 1px solid %BorderStrong%;
		border-radius: %SliderHandleRadius%px;
	}
	QSlider::handle:horizontal:hover { border-color: %AccentBorder%; }
	QSlider::handle:horizontal:pressed { background: %AccentBg%; border-color: %AccentBorder%; }
)";

constexpr char kCheckBoxes[] = R"(
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
	QSplitter::handle { background: transparent; }
	QSplitter::handle:hover, QSplitter::handle:pressed { background: %AccentBg%; }
)";

constexpr char kToolTips[] = R"(
	QToolTip {
		background-color: %BackgroundPrimary%;
		color: %TextPrimary%;
		border: 1px solid %BorderMedium%;
		padding: 4px 8px;
	}
)";

// Per-card stylesheets create a QStyleSheetStyle proxy per widget and make grid rebuilds expensive.
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
		{ QStringLiteral("%SliderHandleContentWidth%"), QString::number(Theme::SliderHandleDiameter - 2) },
		{ QStringLiteral("%SliderHandleOverhang%"),     QString::number((Theme::SliderHandleDiameter - Theme::SliderGrooveThickness) / 2) },
		// QSS url() cannot recolor the per-theme arrow SVG.
		{ QStringLiteral("%ComboArrowIcon%"), Theme::isDark() ? QStringLiteral(":/UI/combobox_down_arrow_dark.svg")
		                                                      : QStringLiteral(":/UI/combobox_down_arrow_light.svg") },
	};
	for (const auto& [token, value] : tokens)
		sheet.replace(token, value);

	return sheet;
}

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

	// Role-only setColor also fills Disabled with full-strength colors.
	const QColor muted(QString::fromLatin1(t.MutedText));
	p.setColor(QPalette::PlaceholderText, muted);
	p.setColor(QPalette::Disabled, QPalette::WindowText, muted);
	p.setColor(QPalette::Disabled, QPalette::Text, muted);
	p.setColor(QPalette::Disabled, QPalette::ButtonText, muted);
	return p;
}

// Qt parents a combo's popup container to the combo and gives it Qt::Popup. Both hold under any name Qt gives
// that private class, which is what makes this a usable cross-check on the name match below.
[[nodiscard]] bool isPopupOwnedByComboBox(const QWidget& w)
{
	return w.windowType() == Qt::Popup && qobject_cast<QComboBox*>(w.parentWidget()) != nullptr;
}

// The private combo popup's opaque viewport defeats QSS border-radius, so paint its surface here.
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
		const bool isComboContainer = container->inherits("QComboBoxPrivateContainer");
		if (type == QEvent::Show)
		{
			// Failing this means Qt renamed the class and none of the popup styling runs any more.
			if (isPopupOwnedByComboBox(*container))
				assert_r(isComboContainer);

			if (isComboContainer)
			{
				container->setAttribute(Qt::WA_TranslucentBackground);
				QAbstractItemView* view = container->findChild<QAbstractItemView*>();
				assert_and_return_r(view != nullptr, QObject::eventFilter(watched, event));
				view->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
				view->viewport()->setAutoFillBackground(false);
			}
		}
		else if (isComboContainer && container->isWindow())
		{
			const Theme::ThemeColors& t = Theme::current();
			QPainter p(container);
			p.setRenderHint(QPainter::Antialiasing);
			const QRectF r = QRectF(container->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
			p.setPen(QPen(QColor(QString::fromLatin1(t.BorderMedium)), 1.0));
			p.setBrush(container->palette().base());
			p.drawRoundedRect(r, Theme::ControlRadius, Theme::ControlRadius);
			return true;
		}
		return QObject::eventFilter(watched, event);
	}
};

// QSplitterHandle does not enable WA_Hover itself, so QSS :hover would never match.
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

// Widen the base style's text-hugging QPushButton focus frame.
class FocusFrameStyle : public QProxyStyle
{
public:
	using QProxyStyle::QProxyStyle;

	void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override
	{
		if (element == PE_FrameFocusRect && qobject_cast<const QPushButton*>(widget))
		{
			if (const auto* focusOption = qstyleoption_cast<const QStyleOptionFocusRect*>(option))
			{
				QStyleOptionFocusRect widened(*focusOption);
				const int pad = Theme::FocusRectOutset;
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
	// Install the proxy before QSS wraps it.
	qApp->setStyle(new FocusFrameStyle);
	qApp->setPalette(paletteFor(Theme::current()));
	qApp->setStyleSheet(styleSheetString());

	qApp->installEventFilter(new ComboPopupRounder(qApp));
	qApp->installEventFilter(new SplitterHandleHoverEnabler(qApp));

	QObject::connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, qApp, [] {
		qApp->setPalette(paletteFor(Theme::current()));
		qApp->setStyleSheet(styleSheetString());
	});
}

void applyThemedSheet(QWidget* widget, std::function<QString()> makeSheet)
{
	widget->setStyleSheet(makeSheet());
	QObject::connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, widget,
	                 [widget, makeSheet = std::move(makeSheet)] { widget->setStyleSheet(makeSheet()); });
}

} // namespace Style
