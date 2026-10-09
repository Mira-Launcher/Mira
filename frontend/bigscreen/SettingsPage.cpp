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
// Rows up to kFirstAction change a value; the rest do something when picked.
enum Row { kButtons, kTextSize, kSounds, kRumble, kAtStart, kOnLogin, kSteam, kSuspend, kRestart, kShutDown, kExit };
constexpr int kFirstAction = kSteam;
constexpr std::array<Setting, 11> kSettings = {{
    {"Button labels", "Which names the button hints use. Match controller reads them from the connected controller."},
    {"Text size", "Large scales everything up by 15%, for a TV across the room."},
    {"Sounds", "Short sounds as you move around and pick things."},
    {"Vibration", "The controller rumbles a little when you pick something or reach the end of a row."},
    {"Open in big screen when Mira starts", "The same as starting Mira with mira-gui --big-screen."},
    {"Start Mira when you log in", "Adds Mira to your desktop's autostart."},
    {"Switch to Steam Big Picture", "Opens Steam's Big Picture. Mira is in its library to come back."},
    {"Suspend", "Puts the computer to sleep."},
    {"Restart", "Restarts the computer."},
    {"Shut down", "Turns the computer off."},
    {"Exit big screen", "Back to the desktop library."},
}};
const std::array<std::pair<const char*, const char*>, 4> kButtonKinds = {
    {{"auto", "Match controller"}, {"xbox", "Xbox"}, {"ps", "PlayStation"}, {"nin", "Nintendo"}}};

}  // namespace

SettingsPage::SettingsPage(BigScreenWindow* window) : Page(window) {}

void SettingsPage::Change(int step) {
  FrontendPrefs prefs = window_->prefs();
  const auto flip = [](std::optional<bool>& value, bool fallback) { value = !value.value_or(fallback); };
  switch (focus_) {
    case kButtons: {
      const std::string current = prefs.big_screen_buttons.value_or("auto");
      const auto at = std::ranges::find(kButtonKinds, current, [](const auto& kind) { return std::string(kind.first); });
      const int index = at == kButtonKinds.end() ? 0 : int(at - kButtonKinds.begin());
      prefs.big_screen_buttons = kButtonKinds[size_t((index + step + int(kButtonKinds.size())) % int(kButtonKinds.size()))].first;
      break;
    }
    case kTextSize: flip(prefs.big_screen_large_text, false); break;
    case kSounds: flip(prefs.big_screen_sounds, true); break;
    case kRumble: flip(prefs.big_screen_rumble, true); break;
    case kAtStart: flip(prefs.big_screen_at_start, false); break;
    case kOnLogin: flip(prefs.start_on_login, false); break;
    default: return;
  }
  window_->SetPrefs(prefs);
}

void SettingsPage::Act() {
  switch (focus_) {
    case kSteam: return window_->OpenSteamBigPicture();
    case kSuspend: return window_->PowerAction("Suspend");
    case kRestart:
      return window_->Confirm("Restart the computer?", "Anything running is closed.", "Restart",
                              [this] { window_->PowerAction("Reboot"); });
    case kShutDown:
      return window_->Confirm("Shut down the computer?", "Anything running is closed.", "Shut down",
                              [this] { window_->PowerAction("PowerOff"); });
    case kExit: return window_->Exit();
    default: return;
  }
}

bool SettingsPage::Navigate(Nav nav) {
  switch (nav) {
    case Nav::Up: focus_ = std::max(0, focus_ - 1); break;
    case Nav::Down: focus_ = std::min(int(kSettings.size()) - 1, focus_ + 1); break;
    case Nav::Left:
    case Nav::Right:
      if (focus_ < kFirstAction) Change(nav == Nav::Right ? 1 : -1);
      break;
    case Nav::Accept:
      if (focus_ >= kFirstAction) {
        Act();
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
  return {{Nav::Accept, focus_ == kExit ? "Exit" : focus_ >= kFirstAction ? "Select" : "Change"}, {Nav::Back, "Back"}};
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
  // Action rows have no value.
  const std::array<QString, kSettings.size()> values = {
      kind == kButtonKinds.end() ? "Match controller" : kind->second,
      prefs.big_screen_large_text.value_or(false) ? "Large" : "Standard",
      prefs.big_screen_sounds.value_or(true) ? "On" : "Off",
      prefs.big_screen_rumble.value_or(true) ? "On" : "Off",
      prefs.big_screen_at_start.value_or(false) ? "On" : "Off",
      prefs.start_on_login.value_or(false) ? "On" : "Off",
  };
  const double list_w = std::min(kListW * u, width() * 0.55);
  // Scrolled so the focused row stays above the hints.
  const double step = kRowH + 0.5;
  const double scroll = std::max(0.0, kTop + 3.2 + (focus_ + 1) * step - (height() / u - 4.5));
  painter.save();
  painter.setClipRect(QRectF(0, (kTop + 2.6) * u, width(), height()));
  for (size_t i = 0; i < kSettings.size(); ++i) {
    const QRectF row(kMargin * u, (kTop + 3.2 + double(i) * step - scroll) * u, list_w, kRowH * u);
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
  painter.restore();
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
