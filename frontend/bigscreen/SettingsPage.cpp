#include "SettingsPage.h"

#include <QPainter>

#include <algorithm>
#include <array>

#include "../theme/Theme.h"
#include "BigScreenWindow.h"
#include "GamepadInput.h"
#include "WebApps.h"

namespace mira_gui::bigscreen {
namespace {

constexpr double kMargin = 2.6, kTop = 6.4, kListW = 46, kRowH = 3.8, kHeadH = 2.6;

// Steps `current` through `options` (value, label) by `step`, wrapping.
template <typename T>
T Cycle(const std::vector<std::pair<T, QString>>& options, const T& current, int step) {
  const auto at = std::ranges::find(options, current, &std::pair<T, QString>::first);
  const int index = at == options.end() ? 0 : int(at - options.begin());
  return options[size_t((index + step + int(options.size())) % int(options.size()))].first;
}

template <typename T>
QString LabelOf(const std::vector<std::pair<T, QString>>& options, const T& current) {
  const auto at = std::ranges::find(options, current, &std::pair<T, QString>::first);
  return at == options.end() ? options.front().second : at->second;
}

const std::vector<std::pair<std::string, QString>> kButtonKinds = {
    {"auto", "Match controller"}, {"xbox", "Xbox"}, {"ps", "PlayStation"}, {"nin", "Nintendo"}};
const std::vector<std::pair<std::string, QString>> kStick = {{"low", "Low"}, {"medium", "Medium"}, {"high", "High"}};
const std::vector<std::pair<std::string, QString>> kRepeat = {{"slow", "Slow"}, {"normal", "Normal"}, {"fast", "Fast"}};
const std::vector<std::pair<std::string, QString>> kRumble = {{"off", "Off"}, {"light", "Light"}, {"strong", "Strong"}};
const std::vector<std::pair<int, QString>> kIdle = {{0, "Never"}, {15, "15 minutes"}, {30, "30 minutes"}, {60, "1 hour"}};

QString OnOff(bool on) { return on ? "On" : "Off"; }

}  // namespace

SettingsPage::SettingsPage(BigScreenWindow* window) : Page(window) {
  test_refresh_.setInterval(30);
  connect(&test_refresh_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  Build();
}

void SettingsPage::Shown() {
  if (page_ >= 0) Close();
  testing_ = false;
  test_refresh_.stop();
  update();
}

void SettingsPage::Build() {
  BigScreenWindow* w = window_;
  // Changes one pref and saves it.
  const auto edit = [w](auto apply) {
    return [w, apply](int step) {
      FrontendPrefs prefs = w->prefs();
      apply(prefs, step);
      w->SetPrefs(prefs);
    };
  };
  const auto flip = [edit](std::optional<bool> FrontendPrefs::*field, bool fallback) {
    return edit([field, fallback](FrontendPrefs& prefs, int) { prefs.*field = !(prefs.*field).value_or(fallback); });
  };
  const auto shows = [w](std::optional<bool> FrontendPrefs::*field, bool fallback) {
    return [w, field, fallback] { return OnOff((w->prefs().*field).value_or(fallback)); };
  };

  const std::vector<Row> controller = {
      {"Buttons", "Button labels", "Which names the button hints use. Match controller reads them from the controller.",
       [w] { return LabelOf(kButtonKinds, w->prefs().big_screen_buttons.value_or("auto")); },
       edit([](FrontendPrefs& p, int step) { p.big_screen_buttons = Cycle(kButtonKinds, p.big_screen_buttons.value_or("auto"), step); }),
       {}},
      {"Buttons", "Select with", "Which face button selects; the other goes back. B selects on Nintendo controllers.",
       [w] { return w->prefs().big_screen_swap_confirm.value_or(false) ? "Right button (B)" : "Bottom button (A)"; },
       flip(&FrontendPrefs::big_screen_swap_confirm, false), {}},
      {"Movement", "Stick sensitivity", "How far the stick moves before it counts. High suits worn sticks less well.",
       [w] { return LabelOf(kStick, w->prefs().big_screen_stick.value_or("medium")); },
       edit([](FrontendPrefs& p, int step) { p.big_screen_stick = Cycle(kStick, p.big_screen_stick.value_or("medium"), step); }),
       {}},
      {"Movement", "Scroll speed", "How fast lists move while a direction is held.",
       [w] { return LabelOf(kRepeat, w->prefs().big_screen_repeat.value_or("normal")); },
       edit([](FrontendPrefs& p, int step) { p.big_screen_repeat = Cycle(kRepeat, p.big_screen_repeat.value_or("normal"), step); }),
       {}},
      {"Feedback", "Vibration", "The controller rumbles when you pick something and knocks at the end of a list.",
       [w] { return LabelOf(kRumble, w->prefs().big_screen_rumble.value_or("light")); },
       edit([](FrontendPrefs& p, int step) { p.big_screen_rumble = Cycle(kRumble, p.big_screen_rumble.value_or("light"), step); }),
       {}},
      {"Test", "Test buttons", "Shows each button as you press it, and the controllers connected. Press B twice to leave.",
       {}, {}, [this] {
         testing_ = true;
         test_refresh_.start();
         update();
         emit HintsChanged();
       }}
  };

  pages_ = {
      {"Controller", controller},
      {"Screen and sound", {
          {"", "Text size", "Large scales everything up by 15%, for a TV across the room.",
           [w] { return w->prefs().big_screen_large_text.value_or(false) ? "Large" : "Standard"; },
           flip(&FrontendPrefs::big_screen_large_text, false), {}},
          {"", "Sounds", "Short sounds as you move around and pick things.", shows(&FrontendPrefs::big_screen_sounds, true),
           flip(&FrontendPrefs::big_screen_sounds, true), {}},
          {"", "Trailers", "A trailer plays behind a game a moment after you rest on it.",
           shows(&FrontendPrefs::big_screen_trailers, true), flip(&FrontendPrefs::big_screen_trailers, true), {}},
          {"", "Trailer sound", "Plays a trailer's sound, not only its picture.",
           shows(&FrontendPrefs::big_screen_trailer_sound, false), flip(&FrontendPrefs::big_screen_trailer_sound, false), {}},
          {"", "Trailer volume", "How loud a trailer plays when its sound is on.",
           [w] { return QString("%1%").arg(w->prefs().big_screen_trailer_volume.value_or(50)); },
           edit([](FrontendPrefs& p, int step) { p.big_screen_trailer_volume = std::clamp(p.big_screen_trailer_volume.value_or(50) + step * 10, 10, 100); }),
           {}}
      }},
      {"Apps", {
          {"", "Show applications", "Lists apps alongside games. Apps tagged media always show, in their own row.",
           shows(&FrontendPrefs::big_screen_show_apps, false), flip(&FrontendPrefs::big_screen_show_apps, false), {}},
          {"", "Add a streaming app", "Netflix, YouTube and others, full screen in your browser with their own sign-in.",
           {}, {}, [w] {
             std::vector<std::pair<QString, std::function<void()>>> entries;
             for (const WebApp& app : StreamingApps()) {
               entries.emplace_back(app.name, [w, app] {
                 AddWebApp(w, app, [w, name = app.name](const QString& error) {
                   w->Toast(error.isEmpty() ? name + " is on Home, in Media" : error);
                 });
               });
             }
             entries.emplace_back("Cancel", [] {});
             w->ShowMenu("Add a streaming app", std::move(entries));
           }},
          {"", "Switch to Steam Big Picture", "Opens Steam's Big Picture. Mira is in its library to come back.", {}, {},
           [w] { w->OpenSteamBigPicture(); }}
      }},
      {"System", {
          {"", "Open in big screen when Mira starts", "The same as starting Mira with mira-gui --big-screen.",
           shows(&FrontendPrefs::big_screen_at_start, false), flip(&FrontendPrefs::big_screen_at_start, false), {}},
          {"", "Start Mira when you log in", "Adds Mira to your desktop's autostart.", shows(&FrontendPrefs::start_on_login, false),
           flip(&FrontendPrefs::start_on_login, false), {}},
          {"", "Sleep when idle", "Suspends the computer after this long on big screen with nothing playing or installing.",
           [w] { return LabelOf(kIdle, w->prefs().big_screen_idle_suspend.value_or(0)); },
           edit([](FrontendPrefs& p, int step) { p.big_screen_idle_suspend = Cycle(kIdle, p.big_screen_idle_suspend.value_or(0), step); }),
           {}},
          {"", "Update system", "Upgrades the system's packages. Your password is asked for, with the on-screen keyboard to type it.",
           [w] { return w->UpdateStatus(); }, {}, [w] { w->UpdateSystem(); }}
      }},
      {"Power", {
          {"", "Suspend", "Puts the computer to sleep.", {}, {}, [w] { w->PowerAction("Suspend"); }},
          {"", "Restart", "Restarts the computer.", {}, {}, [w] {
             w->Confirm("Restart the computer?", "Anything running is closed.", "Restart", [w] { w->PowerAction("Reboot"); });
           }},
          {"", "Shut down", "Turns the computer off.", {}, {}, [w] {
             w->Confirm("Shut down the computer?", "Anything running is closed.", "Shut down", [w] { w->PowerAction("PowerOff"); });
           }}
      }},
  };

  main_rows_ = {
      {"", "Controller", "Button labels, which button selects, stick sensitivity, scroll speed, vibration and a button test.",
       [w] { return w->input().pads().empty() ? QString("No controller") : w->input().pads().front().name; }, {}, {}, 0},
      {"", "Screen and sound", "Text size, sounds and trailers.", {}, {}, {}, 1},
      {"", "Apps", "Show applications, add a streaming app, or switch to Steam Big Picture.", {}, {}, {}, 2},
      {"", "System", "Starting big screen, sleeping when idle, and system updates.", [w] { return w->UpdateStatus(); }, {}, {}, 3},
      {"", "Power", "Suspend, restart or shut down the computer.", {}, {}, {}, 4},
      {"", "Exit big screen", "Back to the desktop library.", {}, {}, [w] { w->Exit(); }},
  };
}

void SettingsPage::Open(int page) {
  main_focus_ = focus_;
  page_ = page;
  focus_ = 0;
}

void SettingsPage::Close() {
  page_ = -1;
  focus_ = main_focus_;
}

bool SettingsPage::Navigate(Nav nav) {
  // Every button is under test; B twice in a row leaves.
  if (testing_) {
    if (nav == Nav::Back && test_back_.isValid() && test_back_.elapsed() < 800) {
      testing_ = false;
      test_refresh_.stop();
      update();
      emit HintsChanged();
    } else if (nav == Nav::Back) {
      test_back_.start();
    }
    return true;
  }
  const std::vector<Row>& rows = this->rows();
  const Row& row = rows[size_t(focus_)];
  switch (nav) {
    case Nav::Up: focus_ = std::max(0, focus_ - 1); break;
    case Nav::Down: focus_ = std::min(int(rows.size()) - 1, focus_ + 1); break;
    case Nav::Left:
    case Nav::Right:
      if (row.change) row.change(nav == Nav::Right ? 1 : -1);
      break;
    case Nav::Accept:
      if (row.page >= 0) Open(row.page);
      else if (row.act) row.act();
      else if (row.change) row.change(1);
      break;
    case Nav::Back:
      if (page_ < 0) return false;
      Close();
      break;
    default: return false;
  }
  update();
  emit HintsChanged();
  return true;
}

QList<Hint> SettingsPage::Hints() const {
  if (testing_) return {};
  const Row& row = rows()[size_t(focus_)];
  const QString accept = row.page >= 0 ? "Open" : !row.act ? "Change" : row.label == "Exit big screen" ? "Exit" : "Select";
  return {{Nav::Accept, accept}, {Nav::Back, page_ >= 0 ? "Settings" : "Back"}};
}

void SettingsPage::PaintTest(QPainter& painter, double u) {
  const theme::Tokens& tokens = theme::Current();
  const GamepadInput& input = window_->input();
  const QString kind = window_->GlyphKind();
  painter.setFont(Font(u, 1.0));
  painter.setPen(tokens.text_muted);
  painter.drawText(QPointF(kMargin * u, (kTop + 4) * u), "Press any button. Press B twice to leave.");
  // Every button as a pill, lit while held.
  static const std::pair<Nav, const char*> kButtons[] = {
      {Nav::Up, "Up"}, {Nav::Down, "Down"}, {Nav::Left, "Left"}, {Nav::Right, "Right"}, {Nav::Accept, ""},
      {Nav::Back, ""}, {Nav::Action, ""}, {Nav::Search, ""}, {Nav::PrevTab, ""}, {Nav::NextTab, ""},
      {Nav::PrevLetter, "Left trigger"}, {Nav::NextLetter, "Right trigger"}, {Nav::Sort, ""}, {Nav::Guide, "Guide"}};
  double x = kMargin * u, y = (kTop + 6.5) * u;
  for (const auto& [nav, label] : kButtons) {
    const QString text = *label != '\0' ? QString(label) : GlyphText(nav, kind);
    painter.setFont(Font(u, 1.1, QFont::Bold));
    const double w = painter.fontMetrics().horizontalAdvance(text) + u * 2.4;
    if (x + w > width() - kMargin * u) {
      x = kMargin * u;
      y += u * 3.6;
    }
    const bool lit = input.down()[size_t(nav)];
    const QRectF box(x, y, w, u * 2.8);
    painter.setPen(lit ? QPen(tokens.text, u * 0.12) : Qt::NoPen);
    painter.setBrush(lit ? Accent() : tokens.surface);
    painter.drawRoundedRect(box, u * 0.5, u * 0.5);
    painter.setPen(tokens.text);
    painter.drawText(box, Qt::AlignCenter, text);
    x += w + u * 0.8;
  }
  // The controllers connected, last used first.
  y += u * 5.5;
  painter.setFont(Font(u, 1.2, QFont::Bold));
  painter.setPen(tokens.text);
  painter.drawText(QPointF(kMargin * u, y), "Controllers");
  painter.setFont(Font(u, 1.0));
  if (!input.available()) {
    painter.setPen(tokens.text_muted);
    painter.drawText(QPointF(kMargin * u, y + u * 2.2), "Controllers need SDL3 (the sdl3 package).");
    return;
  }
  if (input.pads().empty()) {
    painter.setPen(tokens.text_muted);
    painter.drawText(QPointF(kMargin * u, y + u * 2.2), "None connected.");
  }
  for (size_t i = 0; i < input.pads().size(); ++i) {
    const GamepadInput::Pad& pad = input.pads()[i];
    QString line = QString("%1. %2").arg(i + 1).arg(pad.name);
    line += pad.battery >= 0 ? QString(" · %1%%2").arg(pad.battery).arg(pad.charging ? " charging" : "")
                             : pad.wireless ? QString(" · wireless") : QString(" · wired");
    painter.setPen(i == 0 ? tokens.text : tokens.text_muted);
    painter.drawText(QPointF(kMargin * u, y + u * (2.2 + 1.8 * double(i))), line);
  }
}

void SettingsPage::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  painter.fillRect(rect(), QColor(tokens.window.red(), tokens.window.green(), tokens.window.blue(), 170));
  painter.setFont(Font(u, 2.0, QFont::ExtraBold));
  painter.setPen(tokens.text);
  painter.drawText(QPointF(kMargin * u, (kTop + 1.6) * u), testing_ ? "Test buttons" : page_ >= 0 ? pages_[size_t(page_)].title : "Settings");
  if (testing_) return PaintTest(painter, u);

  const std::vector<Row>& rows = this->rows();
  // Rows top to bottom with a heading where each named section starts, in units before scrolling.
  const auto heads = [&rows](size_t i) {
    return !rows[i].section.isEmpty() && (i == 0 || rows[i].section != rows[i - 1].section);
  };
  std::vector<double> tops;
  double y = kTop + 3.2;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (heads(i)) y += kHeadH;
    tops.push_back(y);
    y += kRowH + 0.5;
  }
  // Scrolled so the focused row stays above the hints, with its heading in view at the top.
  const double bottom = height() / u - 4.5;
  const double scroll = std::max(0.0, tops[size_t(focus_)] + kRowH - bottom);
  const double list_w = std::min(kListW * u, width() * 0.55);
  painter.save();
  painter.setClipRect(QRectF(0, (kTop + 2.6) * u, width(), height()));
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    const double top = (tops[i] - scroll) * u;
    if (top > height()) break;
    if (heads(i)) {
      painter.setFont(Font(u, 0.95, QFont::Bold));
      painter.setPen(tokens.text_muted);
      painter.drawText(QPointF(kMargin * u, top - u * 0.8), row.section.toUpper());
    }
    const QRectF box(kMargin * u, top, list_w, kRowH * u);
    const bool focused = int(i) == focus_;
    painter.setPen(focused ? QPen(Accent(), u * 0.12) : Qt::NoPen);
    painter.setBrush(focused ? tokens.surface_alt : tokens.surface);
    painter.drawRoundedRect(box, u * 0.5, u * 0.5);
    const QRectF inner = box.adjusted(u * 1.3, 0, -u * 1.3, 0);
    painter.setFont(Font(u, 1.1, QFont::Bold));
    painter.setPen(tokens.text);
    painter.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft, row.label);
    // Arrows either side where left and right change it, one after where A opens a page.
    QString value = row.value ? row.value() : QString();
    if (row.change) value = "‹  " + value + "  ›";
    else if (row.page >= 0) value = value.isEmpty() ? "›" : value + "   ›";
    if (!value.isEmpty()) {
      painter.setFont(Font(u, 1.0, QFont::DemiBold));
      painter.drawText(inner, Qt::AlignVCenter | Qt::AlignRight, value);
    }
  }
  painter.restore();
  // What the focused setting does.
  const Row& row = rows[size_t(focus_)];
  const QRectF help((kMargin * 2) * u + list_w, (kTop + 3.2) * u, width() - list_w - kMargin * 3 * u, 12 * u);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(255, 255, 255, 12));
  painter.drawRoundedRect(help, u * 0.6, u * 0.6);
  const QRectF help_text = help.adjusted(u * 1.6, u * 1.4, -u * 1.6, -u * 1.4);
  painter.setPen(tokens.text);
  painter.setFont(Font(u, 1.15, QFont::Bold));
  painter.drawText(help_text, Qt::AlignTop | Qt::AlignLeft, row.label);
  painter.setPen(tokens.text_muted);
  painter.setFont(Font(u, 1.0));
  painter.drawText(help_text.adjusted(0, u * 2.2, 0, 0), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, row.help);
}

}  // namespace mira_gui::bigscreen
