#pragma once

#include "Variants.h"

class QWidget;

namespace mira_gui {

// What a piece of text is, which sets its weight and size: a page's or card's heading, a section's
// title, or a small caps-style heading over a group of rows ("PINNED", "SORT").
enum class TextRole { Body, Section, Heading, Group };
enum class TextSize { Normal, Small };

// A label's look on three separate axes, matched by base.qss as the `text_role`, `text_size` and
// `tone` style properties.
struct TextStyle {
  TextRole role = TextRole::Body;
  Tone tone = Tone::Default;
  TextSize size = TextSize::Normal;
};

// The looks in use, by what they're for.
namespace text {
constexpr TextStyle kBody{};
// A line of detail under or beside something: muted and small.
constexpr TextStyle kDetail{TextRole::Body, Tone::Muted, TextSize::Small};
// Muted at the text's own size, e.g. inside a control or a summary.
constexpr TextStyle kSubtle{TextRole::Body, Tone::Muted};
constexpr TextStyle kError{TextRole::Body, Tone::Danger, TextSize::Small};
// Something to notice before confirming, such as a move that copies to another drive.
constexpr TextStyle kWarning{TextRole::Body, Tone::Warning};
constexpr TextStyle kSection{TextRole::Section};
constexpr TextStyle kHeading{TextRole::Heading};
constexpr TextStyle kGroup{TextRole::Group, Tone::Muted, TextSize::Small};
}  // namespace text

// Gives `widget` `style`, restyling it if it's already shown.
void SetTextStyle(QWidget* widget, TextStyle style);

// A link in a label's rich text, `text` going to `href`, both escaped. Drawn in the palette's link
// color (the accent), so it follows a theme change; a link that stands alone is a Link button.
QString Anchor(const QString& href, const QString& text);

}  // namespace mira_gui
