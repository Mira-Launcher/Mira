#include "SettingsPage.h"

#include <QPainter>

#include <algorithm>
#include <array>

#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {
namespace {

constexpr double kMargin = 2.6, kTop = 6.4, kListW = 46, kRowH = 3.8;

struct Setting {
  const char* label;
  const char* help;
};
constexpr std::array<Setting, 4> kSettings = {{
    {"Button labels", "Which names the button hints use. Match controller reads them from the connected controller."},
    {"Text size", "Large scales everything up by 15%, for a TV across the room."},
    {"Open in big screen when Mira starts", "The same as starting Mira with mira-gui --big-screen."},
    {"Exit big screen", "Back to the desktop library."},
}};
const std::array<std::pair<const char*, const char*>, 4> kButtonKinds = {
    {{"auto", "Match controller"}, {"xbox", "Xbox"}, {"ps", "PlayStation"}, {"nin", "Nintendo"}}};

}  // namespace

SettingsPage::SettingsPage(BigScreenWindow* window) : Page(window) {}

void SettingsPage::Change(int step) {
  FrontendPrefs prefs = window_->prefs();
  switch (focus_) {
    case 0: {
      const std::string current = prefs.big_screen_buttons.value_or("auto");
      const auto at = std::ranges::find(kButtonKinds, current, [](const auto& kind) { return std::string(kind.first); });
      const int index = at == kButtonKinds.end() ? 0 : int(at - kButtonKinds.begin());
      prefs.big_screen_buttons = kButtonKinds[size_t((index + step + int(kButtonKinds.size())) % int(kButtonKinds.size()))].first;
      break;
    }
    case 1: prefs.big_screen_large_text = !prefs.big_screen_large_text.value_or(false); break;
    case 2: prefs.big_screen_at_start = !prefs.big_screen_at_start.value_or(false); break;
    default: return;
  }
  window_->SetPrefs(prefs);
}

bool SettingsPage::Navigate(Nav nav) {
  switch (nav) {
    case Nav::Up: focus_ = std::max(0, focus_ - 1); break;
    case Nav::Down: focus_ = std::min(int(kSettings.size()) - 1, focus_ + 1); break;
    case Nav::Left: Change(-1); break;
    case Nav::Right: Change(1); break;
    case Nav::Accept:
      if (focus_ == int(kSettings.size()) - 1) {
        window_->Exit();
        return true;
      }
      Change(1);
      break;
    default: return false;
  }
  update();
  emit HintsChanged();
  return true;
}

QList<Hint> SettingsPage::Hints() const {
  return {{Nav::Accept, focus_ == int(kSettings.size()) - 1 ? "Exit" : "Change"}, {Nav::Back, "Back"}};
}

void SettingsPage::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  const FrontendPrefs& prefs = window_->prefs();
  painter.fillRect(rect(), QColor(tokens.window.red(), tokens.window.green(), tokens.window.blue(), 170));
  painter.setFont(Font(u, 2.0, QFont::ExtraBold));
  painter.setPen(tokens.text);
  painter.drawText(QPointF(kMargin * u, (kTop + 1.6) * u), "Settings");

  const std::string buttons = prefs.big_screen_buttons.value_or("auto");
  const auto kind = std::ranges::find(kButtonKinds, buttons, [](const auto& k) { return std::string(k.first); });
  const QString values[] = {
      kind == kButtonKinds.end() ? "Match controller" : kind->second,
      prefs.big_screen_large_text.value_or(false) ? "Large" : "Standard",
      prefs.big_screen_at_start.value_or(false) ? "On" : "Off",
      QString(),
  };
  const double list_w = std::min(kListW * u, width() * 0.55);
  for (size_t i = 0; i < kSettings.size(); ++i) {
    const QRectF row(kMargin * u, (kTop + 3.2 + double(i) * (kRowH + 0.5)) * u, list_w, kRowH * u);
    const bool focused = int(i) == focus_;
    painter.setPen(focused ? QPen(tokens.accent, u * 0.12) : Qt::NoPen);
    painter.setBrush(focused ? tokens.surface_alt : tokens.surface);
    painter.drawRoundedRect(row, u * 0.5, u * 0.5);
    const QRectF inner = row.adjusted(u * 1.3, 0, -u * 1.3, 0);
    painter.setFont(Font(u, 1.1, QFont::Bold));
    painter.setPen(tokens.text);
    painter.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft, kSettings[i].label);
    if (!values[i].isEmpty()) {
      painter.setFont(Font(u, 1.0, QFont::DemiBold));
      painter.drawText(inner, Qt::AlignVCenter | Qt::AlignRight, "‹  " + values[i] + "  ›");
    }
  }
  // What the focused setting does.
  const QRectF help((kMargin * 2) * u + list_w, (kTop + 3.2) * u, width() - list_w - kMargin * 3 * u, 12 * u);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(255, 255, 255, 12));
  painter.drawRoundedRect(help, u * 0.6, u * 0.6);
  const QRectF help_text = help.adjusted(u * 1.6, u * 1.4, -u * 1.6, -u * 1.4);
  painter.setPen(tokens.text);
  painter.setFont(Font(u, 1.15, QFont::Bold));
  painter.drawText(help_text, Qt::AlignTop | Qt::AlignLeft, kSettings[size_t(focus_)].label);
  painter.setPen(tokens.text_muted);
  painter.setFont(Font(u, 1.0));
  painter.drawText(help_text.adjusted(0, u * 2.2, 0, 0), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                   kSettings[size_t(focus_)].help);
}

}  // namespace mira_gui::bigscreen
