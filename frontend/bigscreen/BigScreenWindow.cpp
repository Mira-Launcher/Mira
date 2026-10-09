#include "BigScreenWindow.h"

#include <linux/input-event-codes.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QWindow>

#include <algorithm>

#include "../client/Jobs.h"
#include "../client/api/Config.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../client/api/Stores.h"
#include "../library/ArtworkStore.h"
#include "../library/GameActions.h"
#include "../library/GameLibraryModel.h"
#include "../library/GameMenus.h"
#include "../library/OwnedTitles.h"
#include "../theme/Theme.h"
#include "DownloadsPage.h"
#include "CollectionsPage.h"
#include "GameKeyboard.h"
#include "GamePage.h"
#include "GamepadInput.h"
#include "HeroBackground.h"
#include "HomePage.h"
#include "Notice.h"
#include "QuickSettings.h"
#include "Screenshots.h"
#include "SearchPage.h"
#include "Session.h"
#include "SettingsPage.h"
#include "Sounds.h"

namespace mira_gui::bigscreen {

Page::Page(BigScreenWindow* window) : QWidget(window), window_(window) {
  setAttribute(Qt::WA_TranslucentBackground);
}

// The top strip (tabs, downloads, clock), the button hints, the toast and a
// confirm dialog, painted over every page.
class Chrome : public QWidget {
public:
  explicit Chrome(BigScreenWindow* window) : QWidget(window), window_(window) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    PaintTop(painter);
    PaintHints(painter);
    PaintToast(painter);
    PaintDialog(painter);
    PaintLaunch(painter);
    PaintMenu(painter);
  }

private:
  void PaintTop(QPainter& painter) {
    const theme::Tokens& tokens = theme::Current();
    const double u = window_->unit();
    const QString kind = window_->GlyphKind();
    QLinearGradient shade(0, 0, 0, u * 4.2);
    shade.setColorAt(0, QColor(0, 0, 0, 150));
    shade.setColorAt(1, QColor(0, 0, 0, 0));
    painter.fillRect(QRectF(0, 0, width(), u * 4.2), shade);
    const double mid = u * 2.1;
    double x = u * 2.6;
    painter.setFont(Font(u, 0.9, QFont::ExtraBold));
    painter.setPen(tokens.text);
    const QString brand = "MIRA";
    painter.drawText(QPointF(x, mid + u * 0.32), brand);
    x += painter.fontMetrics().horizontalAdvance(brand) + u * 1.2;
    x += DrawGlyph(painter, {x, mid}, u, Nav::PrevTab, kind) + u * 0.5;
    const int active = window_->tab_;
    for (int i = 0; i < window_->tab_names_.size(); ++i) {
      painter.setFont(Font(u, 1.05, QFont::DemiBold));
      const QString& name = window_->tab_names_[i];
      const double w = painter.fontMetrics().horizontalAdvance(name) + u * 2;
      const QRectF pill(x, mid - u * 1.05, w, u * 2.1);
      if (i == active) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 22));
        painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
      }
      painter.setPen(i == active ? tokens.text : tokens.text_muted);
      painter.drawText(pill, Qt::AlignCenter, name);
      x += w + u * 0.3;
    }
    x += u * 0.2;
    DrawGlyph(painter, {x, mid}, u, Nav::NextTab, kind);

    // Right side: downloads, controller, clock.
    double right = width() - u * 2.6;
    painter.setFont(Font(u, 0.95, QFont::DemiBold));
    painter.setPen(tokens.text);
    const QString clock = QDateTime::currentDateTime().toString("HH:mm");
    const double clock_w = painter.fontMetrics().horizontalAdvance(clock);
    painter.drawText(QPointF(right - clock_w, mid + u * 0.35), clock);
    right -= clock_w + u * 1.4;
    painter.setFont(Font(u, 0.85));
    painter.setPen(tokens.text_muted);
    const GamepadInput& input = window_->input();
    QString pad = !input.available()        ? "Keyboard · controllers need SDL3"
                  : input.pad_name().isEmpty() ? "No controller"
                                               : input.pad_name();
    if (!input.pads().empty()) {
      const GamepadInput::Pad& first = input.pads().front();
      if (first.battery >= 0) {
        pad += QString(" · %1%2%").arg(first.charging ? "⚡" : "").arg(first.battery);
      } else if (!first.wireless) {
        pad += " · wired";
      }
      if (input.pads().size() > 1) pad += QString("  +%1").arg(input.pads().size() - 1);
    }
    const double pad_w = painter.fontMetrics().horizontalAdvance(pad);
    painter.drawText(QPointF(right - pad_w, mid + u * 0.3), pad);
    right -= pad_w + u * 1.4;
    double total = 0;
    int running = 0;
    for (const DownloadTracker::Entry& entry : window_->services().downloads->Entries()) {
      if (entry.state != DownloadTracker::State::Running || entry.progress < 0) continue;
      total += entry.progress;
      ++running;
    }
    if (running > 0) {
      const double progress = total / running;
      const QRectF bar(right - u * 4, mid - u * 0.15, u * 4, u * 0.3);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(255, 255, 255, 34));
      painter.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
      painter.setBrush(Accent());
      painter.drawRoundedRect(QRectF(bar.topLeft(), QSizeF(bar.width() * progress, bar.height())), bar.height() / 2,
                              bar.height() / 2);
      painter.setPen(tokens.text);
      const QString percent = QString("%1%").arg(qRound(progress * 100));
      const double w = painter.fontMetrics().horizontalAdvance(percent);
      painter.drawText(QPointF(bar.left() - u * 0.5 - w, mid + u * 0.3), percent);
    }
  }

  void PaintHints(QPainter& painter) {
    const double u = window_->unit();
    QList<Hint> hints;
    if (!window_->launching_.key.isEmpty()) {
      hints = {{Nav::Back, "Hide"}};
    } else if (window_->menu_) {
      hints = {{Nav::Accept, "Select"}, {Nav::Back, "Close"}};
    } else if (window_->dialog_) {
      hints = {{Nav::Accept, "Select"}, {Nav::Back, "Cancel"}};
    } else if (auto* page = qobject_cast<Page*>(window_->stack_->currentWidget())) {
      hints = page->Hints();
    }
    QLinearGradient shade(0, height(), 0, height() - u * 3.4);
    shade.setColorAt(0, QColor(0, 0, 0, 170));
    shade.setColorAt(1, QColor(0, 0, 0, 0));
    painter.fillRect(QRectF(0, height() - u * 3.4, width(), u * 3.4), shade);
    painter.setFont(Font(u, 0.95));
    double x = width() - u * 2.6;
    const double mid = height() - u * 1.7;
    for (auto it = hints.crbegin(); it != hints.crend(); ++it) {
      const double text_w = painter.fontMetrics().horizontalAdvance(it->label);
      x -= text_w;
      painter.setPen(theme::Current().text);
      painter.drawText(QPointF(x, mid + u * 0.33), it->label);
      const double glyph_w = GlyphWidth(u, it->nav, window_->GlyphKind());
      x -= glyph_w + u * 0.45;
      DrawGlyph(painter, {x, mid}, u, it->nav, window_->GlyphKind());
      x -= u * 1.6;
    }
  }

  void PaintToast(QPainter& painter) {
    if (window_->toast_.isEmpty()) return;
    const theme::Tokens& tokens = theme::Current();
    const double u = window_->unit();
    painter.setFont(Font(u, 1.0, QFont::DemiBold));
    const double w = painter.fontMetrics().horizontalAdvance(window_->toast_) + u * 2.6;
    const QRectF box((width() - w) / 2, height() - u * 7, w, u * 2.6);
    painter.setPen(QPen(tokens.border, 1));
    painter.setBrush(tokens.surface_alt);
    painter.drawRoundedRect(box, u * 0.5, u * 0.5);
    painter.setPen(tokens.text);
    painter.drawText(box, Qt::AlignCenter, window_->toast_);
  }

  void PaintMenu(QPainter& painter) {
    if (!window_->menu_) return;
    const BigScreenWindow::Menu& menu = *window_->menu_;
    const theme::Tokens& tokens = theme::Current();
    const double u = window_->unit();
    painter.fillRect(rect(), QColor(0, 0, 0, 170));
    const double row_h = u * 3.4;
    const QRectF box((width() - u * 30) / 2, (height() - row_h * double(menu.entries.size()) - u * 6) / 2, u * 30,
                     row_h * double(menu.entries.size()) + u * 6);
    painter.setPen(QPen(tokens.border, 1));
    painter.setBrush(tokens.surface);
    painter.drawRoundedRect(box, u * 0.7, u * 0.7);
    painter.setPen(tokens.text_muted);
    painter.setFont(Font(u, 1.0, QFont::DemiBold));
    painter.drawText(box.adjusted(u * 2, u * 1.4, -u * 2, 0), Qt::AlignTop | Qt::AlignLeft, menu.title);
    painter.setFont(Font(u, 1.2, QFont::Bold));
    for (size_t i = 0; i < menu.entries.size(); ++i) {
      const QRectF row(box.left() + u * 1.2, box.top() + u * 4 + double(i) * row_h, box.width() - u * 2.4, row_h - u * 0.4);
      const bool focused = int(i) == menu.focus;
      painter.setPen(focused ? QPen(Accent(), u * 0.12) : Qt::NoPen);
      painter.setBrush(focused ? tokens.surface_alt : Qt::transparent);
      painter.drawRoundedRect(row, u * 0.45, u * 0.45);
      painter.setPen(tokens.text);
      painter.drawText(row.adjusted(u * 1.2, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, menu.entries[i].first);
    }
  }

  void PaintDialog(QPainter& painter) {
    if (!window_->dialog_) return;
    const BigScreenWindow::Dialog& dialog = *window_->dialog_;
    const theme::Tokens& tokens = theme::Current();
    const double u = window_->unit();
    painter.fillRect(rect(), QColor(0, 0, 0, 180));
    const QRectF box((width() - u * 34) / 2, (height() - u * 14) / 2, u * 34, u * 14);
    painter.setPen(QPen(tokens.border, 1));
    painter.setBrush(tokens.surface);
    painter.drawRoundedRect(box, u * 0.7, u * 0.7);
    const QRectF inner = box.adjusted(u * 2, u * 1.8, -u * 2, -u * 1.8);
    painter.setPen(tokens.text);
    painter.setFont(Font(u, 1.6, QFont::Bold));
    painter.drawText(inner, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, dialog.title);
    painter.setFont(Font(u, 1.0));
    painter.setPen(tokens.text_muted);
    painter.drawText(inner.adjusted(0, u * 2.8, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, dialog.body);
    // Buttons, right-aligned: confirm then cancel.
    painter.setFont(Font(u, 1.05, QFont::Bold));
    const QStringList labels = {dialog.confirm_label, "Cancel"};
    double x = inner.right();
    for (int i = int(labels.size()) - 1; i >= 0; --i) {
      const double w = painter.fontMetrics().horizontalAdvance(labels[i]) + u * 2.8;
      x -= w;
      const QRectF button(x, inner.bottom() - u * 2.6, w, u * 2.6);
      const bool focused = dialog.focus == i;
      painter.setPen(focused ? QPen(tokens.text, u * 0.12) : Qt::NoPen);
      painter.setBrush(focused && i == 0 ? tokens.error : QColor(255, 255, 255, 26));
      painter.drawRoundedRect(button, u * 0.45, u * 0.45);
      painter.setPen(i == 0 && !focused ? tokens.error.lighter(140) : tokens.text);
      painter.drawText(button, Qt::AlignCenter, labels[i]);
      x -= u * 0.8;
    }
  }

  // While a game starts: its cover and name over a dimmed screen, so a slow
  // Steam start doesn't look like nothing happened.
  void PaintLaunch(QPainter& painter) {
    const Item& item = window_->launching_;
    if (item.key.isEmpty()) return;
    const theme::Tokens& tokens = theme::Current();
    const double u = window_->unit();
    painter.fillRect(QRectF(0, u * 4.2, width(), height() - u * 7.6), QColor(0, 0, 0, 200));
    const QSize cover(qRound(12 * u), qRound(18 * u));
    const QRectF box((width() - cover.width()) / 2.0, height() / 2.0 - u * 14, cover.width(), cover.height());
    DrawCover(painter, box, window_->Cover(item, cover), u, false, false, -1);
    painter.setPen(tokens.text);
    painter.setFont(Font(u, 2.0, QFont::Bold));
    const QRectF title(0, box.bottom() + u * 1.4, width(), u * 3);
    painter.drawText(title, Qt::AlignHCenter | Qt::AlignTop, "Starting " + item.name);
    painter.setPen(tokens.text_muted);
    painter.setFont(Font(u, 1.05));
    // Nothing reports a prefix being set up, but a Windows game's first start is when it happens.
    const bool first_run = item.game && !item.game->last_played_at && item.game->play_seconds == 0;
    const QString detail = item.game && item.game->source == "steam" ? "Steam is starting it. This can take a moment."
                           : first_run ? "Its first start sets things up, which can take a minute or two."
                                       : "This can take a moment.";
    painter.drawText(title.translated(0, u * 3), Qt::AlignHCenter | Qt::AlignTop, detail);
  }

  BigScreenWindow* window_;
};

BigScreenWindow::BigScreenWindow(LibraryServices services, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), services_(services) {
  setWindowTitle("Mira");
  setFocusPolicy(Qt::StrongFocus);
  // Driven by a controller or keys; a pointer would only sit over the art.
  setCursor(Qt::BlankCursor);
  input_ = new GamepadInput(this);
  hero_ = new HeroBackground(this);
  stack_ = new QStackedWidget(this);
  stack_->setAttribute(Qt::WA_TranslucentBackground);
  tabs_ = {new HomePage(this), new CollectionsPage(this), new SearchPage(this), new DownloadsPage(this),
           new SettingsPage(this)};
  tab_names_ = {"Home", "Collections", "Search", "Downloads", "Settings"};
  for (Page* page : tabs_) stack_->addWidget(page);
  game_page_ = new GamePage(this);
  stack_->addWidget(game_page_);
  for (Page* page : tabs_ + QList<Page*>{game_page_}) {
    connect(page, &Page::HintsChanged, this, [this] { chrome_->update(); });
  }
  chrome_ = new Chrome(this);

  sounds_ = new Sounds(this);
  keyboard_ = new GameKeyboard(this);
  notice_ = new Notice(this);
  connect(input_, &GamepadInput::Pressed, this, [this](Nav nav) {
    // The keyboard over a game takes every button while it's up.
    if (keyboard_->isVisible()) return keyboard_->Navigate(nav);
    if (nav == Nav::Guide) {
      guide_chord_ = false;
      guide_hold_.start();
      return;
    }
    if (guide_hold_.isActive() && nav == Nav::Accept) {
      guide_chord_ = true;
      if (const GameSummary* game = RunningGame()) Screenshot(QString::fromStdString(game->name));
      return;
    }
    if (!isActiveWindow()) {
      // A game in front has the controller; an app gets it as keys.
      if (AppInFront()) AppControl(nav);
      return;
    }
    Navigate(nav);
  });
  guide_hold_.setInterval(20);
  connect(&guide_hold_, &QTimer::timeout, this, [this] {
    if (input_->down()[size_t(Nav::Guide)]) return;
    guide_hold_.stop();
    if (!guide_chord_) Guide();
  });
  connect(input_, &GamepadInput::Activity, &idle_, qOverload<>(&QTimer::start));
  connect(input_, &GamepadInput::BatteryLow, this, [this](const QString& name, int percent) {
    Tell(QString("%1 is at %2%").arg(name).arg(percent), "Charge it soon.");
  });
  idle_.setSingleShot(true);
  connect(&idle_, &QTimer::timeout, this, [this] {
    // Only from a quiet Home: nothing playing, nothing installing.
    const bool busy = RunningGame() != nullptr || !installing_.isEmpty() || !isActiveWindow();
    if (!busy) PowerAction("Suspend");
  });
  auto* sleep = new SleepWatcher(this);
  connect(sleep, &SleepWatcher::Woke, this, [this] {
    if (RunningGame() == nullptr) RaiseFromGame();
    idle_.start();
  });
  connect(hero_, &HeroBackground::ArtChanged, this, [this] {
    stack_->currentWidget()->update();
    chrome_->update();
  });
  connect(services_.downloads, &DownloadTracker::Changed, this, &BigScreenWindow::FollowDownloads);
  blanking_cookie_ = InhibitScreenBlanking();
  connect(input_, &GamepadInput::PadChanged, chrome_, qOverload<>(&QWidget::update));
  connect(services_.downloads, &DownloadTracker::Changed, chrome_, qOverload<>(&QWidget::update));
  toast_timer_.setSingleShot(true);
  toast_timer_.setInterval(2600);
  connect(&toast_timer_, &QTimer::timeout, this, [this] {
    toast_.clear();
    chrome_->update();
  });
  launch_timeout_.setSingleShot(true);
  connect(&launch_timeout_, &QTimer::timeout, this, [this] {
    launching_ = {};
    chrome_->update();
  });
  connect(services_.library, &GameLibraryModel::Changed, this, &BigScreenWindow::FollowLaunch);
  clock_timer_.setInterval(10'000);
  connect(&clock_timer_, &QTimer::timeout, chrome_, qOverload<>(&QWidget::update));
  clock_timer_.start();

  api::GetFrontendPrefsAsync(this, [this](FrontendPrefsResult result) {
    if (!result.ok) return;
    prefs_ = result.prefs;
    ApplyInputOptions();
    for (Page* page : tabs_) page->Shown();
    hero_->update();
    chrome_->update();
  });
  services_.owned_titles->RefreshIfStale();
  SelectTab(0);
  if (!input_->available()) Toast("Controllers need SDL3 (the sdl3 package). The keyboard works.");
}

BigScreenWindow::~BigScreenWindow() {
  ReleaseScreenBlanking(blanking_cookie_);
  delete keyboard_;
  delete notice_;
}

void BigScreenWindow::SetPrefs(const FrontendPrefs& prefs) {
  prefs_ = prefs;
  FrontendPrefs saved;
  saved.big_screen_buttons = prefs.big_screen_buttons;
  saved.big_screen_large_text = prefs.big_screen_large_text;
  saved.big_screen_at_start = prefs.big_screen_at_start;
  saved.start_on_login = prefs.start_on_login;
  saved.big_screen_sounds = prefs.big_screen_sounds;
  saved.big_screen_rumble = prefs.big_screen_rumble;
  saved.big_screen_show_apps = prefs.big_screen_show_apps;
  saved.big_screen_swap_confirm = prefs.big_screen_swap_confirm;
  saved.big_screen_stick = prefs.big_screen_stick;
  saved.big_screen_repeat = prefs.big_screen_repeat;
  saved.big_screen_trailers = prefs.big_screen_trailers;
  saved.big_screen_idle_suspend = prefs.big_screen_idle_suspend;
  saved.big_screen_recent_searches = prefs.big_screen_recent_searches;
  ApplyInputOptions();
  ApplyStartOnLogin(prefs.start_on_login.value_or(false));
  api::SaveFrontendPrefsAsync(this, saved, [](PatchConfigResult) {});
  for (Page* page : tabs_) page->Shown();
  update();
  stack_->currentWidget()->update();
  chrome_->update();
}

double BigScreenWindow::unit() const { return Unit(height(), prefs_.big_screen_large_text.value_or(false)); }

QString BigScreenWindow::GlyphKind() const {
  const std::string chosen = prefs_.big_screen_buttons.value_or("auto");
  if (chosen != "auto") return QString::fromStdString(chosen);
  return input_->pad_kind().isEmpty() ? "xbox" : input_->pad_kind();
}

Item BigScreenWindow::ItemFor(const GameSummary& game) const {
  return {QString::fromStdString(game.id), QString::fromStdString(game.name), QString::fromStdString(game.source), {},
          game};
}

Item BigScreenWindow::ItemFor(const StoreTitle& title) const {
  const QString source = QString::fromStdString(title.source);
  const QString ref = QString::fromStdString(title.ref);
  return {source + "-" + ref, QString::fromStdString(title.title), source, ref, std::nullopt};
}

Item BigScreenWindow::Find(const QString& key) const {
  if (const GameSummary* game = services_.library->Find(key.toStdString())) return ItemFor(*game);
  for (const StoreTitle& title : services_.owned_titles->Titles()) {
    if (QString::fromStdString(title.source + "-" + title.ref) == key) return ItemFor(title);
  }
  return {};
}

bool BigScreenWindow::Browsable(const GameSummary& game) const {
  // Media apps (tagged media) stay, so Netflix or Stremio are there with apps hidden.
  return !IsHidden(game) &&
         (!IsApp(game) || prefs_.big_screen_show_apps.value_or(false) || HasTag(game, "media"));
}

bool BigScreenWindow::Browsable(const StoreTitle& title) const {
  // Microsoft 365 is the one store whose titles are applications.
  return title.source != "office" || prefs_.big_screen_show_apps.value_or(false);
}

QPixmap BigScreenWindow::Logo(const Item& item) const { return hero_->Logo(item.key); }

QPixmap BigScreenWindow::Cover(const Item& item, QSize size) const {
  const qreal dpr = devicePixelRatioF();
  QPixmap cover;
  if (item.game) cover = services_.artwork->Cover(*item.game, size, dpr);
  if (!item.game && !item.ref.isEmpty()) cover = services_.artwork->TitleCover(item.source, item.ref, item.name, size, dpr);
  // ArtworkStore drops a store title's scaled copy once nothing else holds it, and would fetch
  // it again on every repaint.
  shown_covers_.insert(item.key + '@' + QString::number(size.width()), cover);
  return cover;
}

const DownloadTracker::Entry* BigScreenWindow::Download(const Item& item) const {
  using Kind = DownloadTracker::Kind;
  // A store game ("<source>-<ref>") updates as its store title.
  QString ref = item.ref;
  if (ref.isEmpty() && item.key.startsWith(item.source + "-")) ref = item.key.mid(item.source.size() + 1);
  const DownloadTracker::Entry* entry =
      ref.isEmpty() ? nullptr : services_.downloads->Find(DownloadTracker::KeyFor(Kind::Title, item.source, ref));
  if (entry == nullptr) entry = services_.downloads->Find(DownloadTracker::KeyFor(Kind::Game, {}, item.key));
  if (entry == nullptr) return nullptr;
  const bool live = entry->state == DownloadTracker::State::Running || entry->state == DownloadTracker::State::Paused;
  return live ? entry : nullptr;
}

double BigScreenWindow::Progress(const Item& item) const {
  const DownloadTracker::Entry* entry = Download(item);
  if (entry == nullptr) return -1;
  return std::max(0.0, entry->progress);
}

void BigScreenWindow::ShowHero(const Item& item) { hero_->Show(item); }

void BigScreenWindow::OpenGame(const Item& item) {
  game_page_->Open(item, qobject_cast<Page*>(stack_->currentWidget()));
  ShowPage(game_page_);
}

void BigScreenWindow::Toast(const QString& text) {
  toast_ = text;
  toast_timer_.start();
  chrome_->update();
}

void BigScreenWindow::Confirm(const QString& title, const QString& body, const QString& confirm_label,
                              std::function<void()> on_confirm) {
  dialog_ = Dialog{title, body, confirm_label, std::move(on_confirm)};
  chrome_->update();
}

void BigScreenWindow::Play(const Item& item) {
  if (!item.game) return;
  launching_ = item;
  // Steam may need a while to start, update or sync first.
  launch_timeout_.start(120'000);
  chrome_->update();
  actions::Launch(
      this, item.game->id,
      [this](bool tracked) {
        // Nothing will say an untracked game started: the overlay just goes after a moment.
        if (!tracked) {
          handed_off_ = true;
          launch_timeout_.start(8000);
        }
      },
      [this] {
        launching_ = {};
        chrome_->update();
      });
}

void BigScreenWindow::FollowLaunch() {
  if (!launching_.key.isEmpty()) {
    const GameSummary* game = services_.library->Find(launching_.key.toStdString());
    if (game != nullptr && game->running) {
      playing_ = launching_.key;
      launching_ = {};
      launch_timeout_.stop();
      chrome_->update();
    }
  } else if (!playing_.isEmpty()) {
    const GameSummary* game = services_.library->Find(playing_.toStdString());
    if (game == nullptr || !game->running) {
      playing_.clear();
      RaiseFromGame();
    }
  }
}

void BigScreenWindow::Stop(const Item& item) {
  if (item.game) actions::Stop(this, item.game->id);
}

void BigScreenWindow::Install(const Item& item) {
  const auto done = [this, name = item.name](const ApiError& error) {
    Toast(error.message.empty() ? "Installing " + name : "Could not install " + name);
  };
  if (!item.ref.isEmpty()) {
    api::InstallStoreTitleAsync(this, item.source.toStdString(), item.ref.toStdString(), false,
                                [done](StoreActionResult result) { done(result.error); });
  } else if (item.game && item.game->status == "needs_install") {
    api::InstallGameAsync(this, item.game->id, false, {}, [done](GameActionResult result) { done(result.error); });
  }
}

void BigScreenWindow::Pause(const Item& item) {
  const DownloadTracker::Entry* entry = Download(item);
  if (entry == nullptr) return;
  api::PauseStoreInstallAsync(this, entry->source.toStdString(), entry->ref.toStdString(),
                              [this, name = item.name](StoreActionResult result) {
                                Toast(result.ok ? "Paused " + name : "Could not pause " + name);
                              });
}

void BigScreenWindow::Resume(const Item& item) {
  const DownloadTracker::Entry* entry = Download(item);
  if (entry == nullptr) return;
  api::InstallStoreTitleAsync(this, entry->source.toStdString(), entry->ref.toStdString(), entry->update,
                              [this, name = item.name](StoreActionResult result) {
                                if (!result.ok) Toast("Could not resume " + name);
                              });
}

void BigScreenWindow::CancelInstall(const Item& item) {
  const DownloadTracker::Entry* entry = Download(item);
  if (entry == nullptr) return;
  if (entry->state == DownloadTracker::State::Paused) {
    api::DiscardPausedInstallAsync(this, entry->source.toStdString(), entry->ref.toStdString(), [](StoreActionResult) {});
  } else if (const QString job = services_.downloads->JobFor(*entry); !job.isEmpty()) {
    jobs::Cancel(this, job.toStdString(), [](ApiError) {});
  }
  Toast("Canceled " + item.name);
}

void BigScreenWindow::Uninstall(const Item& item) {
  if (!item.game) return;
  Confirm("Uninstall " + item.name + "?",
          "This deletes the game's files. Its Wine prefix, with saves and settings, is kept.", "Uninstall",
          [this, item] {
            api::DeleteGameAsync(this, item.game->id, true, false, false, [this, name = item.name](DeleteResult result) {
              Toast(result.ok ? name + " uninstalled" : "Could not uninstall " + name);
            });
          });
}

void BigScreenWindow::TogglePin(const Item& item) {
  if (item.game) services_.menus->ToggleTag(item.game->id, tags::kPinned);
}

QString BigScreenWindow::QuickActionLabel(const Item& item) const {
  if (Download(item) != nullptr) return {};
  if (!item.game) return "Install";
  if (item.game->running) return "Stop";
  return item.game->status == "needs_install" ? "Install" : "Play";
}

void BigScreenWindow::QuickAction(const Item& item) {
  const QString label = QuickActionLabel(item);
  if (label == "Play") Play(item);
  if (label == "Stop") Stop(item);
  if (label == "Install") Install(item);
}

void BigScreenWindow::Exit() { close(); }

void BigScreenWindow::ShowMenu(const QString& title, std::vector<std::pair<QString, std::function<void()>>> entries) {
  menu_ = Menu{title, std::move(entries)};
  chrome_->update();
}

void BigScreenWindow::OpenSteamBigPicture() {
  Toast("Opening Steam Big Picture");
  // Steam takes the screen; staying behind it until the player comes back.
  handed_off_ = true;
  api::OpenSteamBigPictureAsync(this, [this](StoreActionResult result) {
    if (result.ok) return;
    handed_off_ = false;
    Toast("Couldn't open Steam: " + QString::fromStdString(result.error.message));
  });
}

bool BigScreenWindow::event(QEvent* event) {
  if (event->type() == QEvent::WindowActivate) handed_off_ = false;
  if (event->type() == QEvent::WindowDeactivate) {
    // Something like Steam's own window came over big screen while no game
    // runs: come back on top.
    QTimer::singleShot(300, this, [this] {
      if (isActiveWindow() || QApplication::activeWindow() != nullptr || handed_off_ || !launching_.key.isEmpty()) return;
      const auto& games = services_.library->Games();
      if (std::ranges::any_of(games, &GameSummary::running)) return;
      RaiseFromGame();
    });
  }
  return QWidget::event(event);
}

void BigScreenWindow::closeEvent(QCloseEvent* event) {
  emit Closed();
  QWidget::closeEvent(event);
}

void BigScreenWindow::Navigate(Nav nav) {
  Feedback(nav);
  if (nav == Nav::Home) {
    menu_.reset();
    dialog_.reset();
    return SelectTab(kHomeTab);
  }
  if (menu_) {
    const int count = int(menu_->entries.size());
    if (nav == Nav::Up || nav == Nav::Down) menu_->focus = (menu_->focus + (nav == Nav::Down ? 1 : count - 1)) % count;
    if (nav == Nav::Back) menu_.reset();
    if (nav == Nav::Accept) {
      auto action = std::move(menu_->entries[size_t(menu_->focus)].second);
      menu_.reset();
      action();
    }
    chrome_->update();
    return;
  }
  if (!launching_.key.isEmpty()) {
    if (nav == Nav::Back) {
      launching_ = {};
      launch_timeout_.stop();
      chrome_->update();
    }
    return;
  }
  if (dialog_) {
    if (nav == Nav::Left || nav == Nav::Right) dialog_->focus = nav == Nav::Left ? 0 : 1;
    if (nav == Nav::Back) dialog_.reset();
    if (nav == Nav::Accept) {
      auto on_confirm = dialog_->focus == 0 ? std::move(dialog_->on_confirm) : nullptr;
      dialog_.reset();
      if (on_confirm) on_confirm();
    }
    chrome_->update();
    return;
  }
  auto* page = qobject_cast<Page*>(stack_->currentWidget());
  if (page != nullptr && page->Navigate(nav)) return;
  switch (nav) {
    case Nav::PrevTab: return SelectTab((tab_ + int(tabs_.size()) - 1) % int(tabs_.size()));
    case Nav::NextTab: return SelectTab((tab_ + 1) % int(tabs_.size()));
    case Nav::Search: return SelectTab(kSearchTab);
    case Nav::Back: return Back();
    default: return;
  }
}

void BigScreenWindow::SelectTab(int index) {
  tab_ = index;
  ShowPage(tabs_[index]);
}

void BigScreenWindow::ShowPage(Page* page) {
  stack_->setCurrentWidget(page);
  page->Shown();
  chrome_->update();
}

void BigScreenWindow::Back() {
  if (stack_->currentWidget() == game_page_) return ShowPage(game_page_->from() ? game_page_->from() : tabs_[tab_]);
  if (tab_ != 0) SelectTab(0);
}

void BigScreenWindow::Feedback(Nav nav) {
  const bool directions = nav == Nav::Up || nav == Nav::Down || nav == Nav::Left || nav == Nav::Right;
  if (prefs_.big_screen_sounds.value_or(true)) {
    sounds_->Play(directions        ? Sounds::Cue::Move
                  : nav == Nav::Back ? Sounds::Cue::Back
                                     : Sounds::Cue::Accept);
  }
  const std::string rumble = prefs_.big_screen_rumble.value_or("light");
  if (rumble != "off" && nav == Nav::Accept) input_->Rumble(rumble == "strong" ? 0.4 : 0.15, rumble == "strong" ? 0.8 : 0.35, 45);
}

void BigScreenWindow::Bump() {
  if (prefs_.big_screen_sounds.value_or(true)) sounds_->Play(Sounds::Cue::Bump);
  const std::string rumble = prefs_.big_screen_rumble.value_or("light");
  if (rumble != "off") input_->Rumble(rumble == "strong" ? 0.9 : 0.45, 0.1, 70);
}

const GameSummary* BigScreenWindow::RunningGame() const {
  for (const GameSummary& game : services_.library->Games()) {
    if (game.running) return &game;
  }
  return nullptr;
}

void BigScreenWindow::Guide() {
  if (menu_ && isActiveWindow()) {
    menu_.reset();
    return ReturnToGame();
  }
  RaiseFromGame();
  const GameSummary* game = RunningGame();
  // Nothing to go back to: Guide just brings big screen up.
  if (game == nullptr && !handed_off_) return;
  Menu menu{game != nullptr ? QString::fromStdString(game->name) : "Game"};
  menu.entries.emplace_back("Return to game", [this] { ReturnToGame(); });
  menu.entries.emplace_back("Keyboard", [this] { OpenGameKeyboard(); });
  const QString shot_name = game != nullptr ? QString::fromStdString(game->name) : "Game";
  menu.entries.emplace_back("Take a screenshot", [this, shot_name] {
    ReturnToGame();
    // Once the game is back on screen.
    QTimer::singleShot(700, this, [this, shot_name] { Screenshot(shot_name); });
  });
  if (game != nullptr) {
    const std::string id = game->id;
    const QString name = QString::fromStdString(game->name);
    menu.entries.emplace_back("Performance overlay", [this, id] { TogglePerformanceOverlay(id); });
    menu.entries.emplace_back("Game settings", [this, id, name] { OpenQuickSettings(id, name); });
    menu.entries.emplace_back("Stop " + name, [this, id, name] {
      Confirm("Stop " + name + "?", "Anything not saved in the game is lost.", "Stop",
              [this, id] { actions::Stop(this, id); });
    });
  }
  menu.entries.emplace_back("Downloads", [this] { SelectTab(kDownloadsTab); });
  menu_ = std::move(menu);
  chrome_->update();
}

void BigScreenWindow::ReturnToGame() {
  // Out of the way, so the window manager hands focus back to the game.
  handed_off_ = true;
  showMinimized();
}

void BigScreenWindow::OpenGameKeyboard() {
  ReturnToGame();
  // After the game has the screen again, so the keyboard lands over it.
  QTimer::singleShot(400, this, [this] {
    QString error;
    if (!keyboard_->Open(&error)) {
      RaiseFromGame();
      Toast(error);
    }
  });
}

void BigScreenWindow::Tell(const QString& title, const QString& detail) {
  if (isActiveWindow()) return Toast(detail.isEmpty() ? title : title + ". " + detail);
  notice_->Show(title, detail);
}

void BigScreenWindow::FollowDownloads() {
  QSet<QString> now;
  for (const DownloadTracker::Entry& entry : services_.downloads->Entries()) {
    if (entry.state == DownloadTracker::State::Running) now.insert(entry.key);
    if (entry.state == DownloadTracker::State::Finished && installing_.contains(entry.key)) {
      Tell(services_.downloads->NameFor(entry) + " is installed");
    }
  }
  installing_ = now;
}

void BigScreenWindow::Screenshot(const QString& game_name) {
  TakeScreenshot(this, game_name, [this, game_name](const QString& path, const QString& error) {
    if (path.isEmpty()) return Tell("Couldn't take a screenshot", error);
    Tell("Screenshot saved", "Pictures/Mira/" + QFileInfo(path).dir().dirName());
  });
}

void BigScreenWindow::TogglePerformanceOverlay(const std::string& id) {
  LoadQuickSettings(this, id, [this, id](bool ok, QuickSettings settings) {
    if (!ok) return Toast("Couldn't read the game's settings.");
    if (settings.overlay || settings.fps_limit > 0) {
      // MangoHud is loaded: its own toggle, right Shift + F12.
      ReturnToGame();
      QTimer::singleShot(400, this, [this] {
        QString error;
        if (!keyboard_->Press(KEY_F12, &error, KEY_RIGHTSHIFT)) Tell("Couldn't toggle the overlay", error);
      });
      return;
    }
    settings.overlay = true;
    SaveQuickSettings(this, id, settings, [this](const QString& error) {
      Toast(error.isEmpty() ? "The overlay shows from the next start." : error);
    });
  });
}

void BigScreenWindow::OpenQuickSettings(const std::string& id, const QString& name) {
  LoadQuickSettings(this, id, [this, id, name](bool ok, QuickSettings settings) {
    if (!ok) return Toast("Couldn't read the game's settings.");
    ShowQuickSettings(id, name, settings);
  });
}

void BigScreenWindow::ShowQuickSettings(const std::string& id, const QString& name, const QuickSettings& settings) {
  Menu menu{name + " · from its next start"};
  const auto change = [this, id, name](QuickSettings next) {
    ShowQuickSettings(id, name, next);
    SaveQuickSettings(this, id, next, [this](const QString& error) {
      if (!error.isEmpty()) Toast(error);
    });
  };
  static const int kLimits[] = {0, 30, 40, 60, 90, 120};
  menu.entries.emplace_back(
      "Frame limit: " + (settings.fps_limit > 0 ? QString::number(settings.fps_limit) + " fps" : QString("Off")),
      [settings, change] {
        QuickSettings next = settings;
        const auto at = std::ranges::find(kLimits, settings.fps_limit);
        next.fps_limit = at == std::end(kLimits) || at + 1 == std::end(kLimits) ? 0 : *(at + 1);
        change(next);
      });
  menu.entries.emplace_back(QString("Performance overlay: ") + (settings.overlay ? "On" : "Off"), [settings, change] {
    QuickSettings next = settings;
    next.overlay = !next.overlay;
    change(next);
  });
  menu.entries.emplace_back(QString("GameMode: ") + (settings.gamemode ? "On" : "Off"), [settings, change] {
    QuickSettings next = settings;
    next.gamemode = !next.gamemode;
    change(next);
  });
  menu.entries.emplace_back("Done", [] {});
  menu_ = std::move(menu);
  chrome_->update();
}

bool BigScreenWindow::AppInFront() const {
  const GameSummary* game = RunningGame();
  return game != nullptr ? IsApp(*game) : launching_.game && IsApp(*launching_.game);
}

void BigScreenWindow::AppControl(Nav nav) {
  // TV-style apps (Stremio, a streaming site) take arrows, Enter, Escape and Space.
  static const std::map<Nav, int> kKeys = {
      {Nav::Up, KEY_UP},         {Nav::Down, KEY_DOWN}, {Nav::Left, KEY_LEFT},    {Nav::Right, KEY_RIGHT},
      {Nav::Accept, KEY_ENTER},  {Nav::Back, KEY_ESC},  {Nav::Action, KEY_SPACE},
  };
  if (nav == Nav::Search) return OpenGameKeyboard();
  const auto found = kKeys.find(nav);
  if (found == kKeys.end()) return;
  QString error;
  if (!keyboard_->Press(found->second, &error)) Tell("Couldn't control the app", error);
}

void BigScreenWindow::ApplyInputOptions() {
  GamepadInput::Options options;
  options.swap_confirm = prefs_.big_screen_swap_confirm.value_or(false);
  const std::string stick = prefs_.big_screen_stick.value_or("medium");
  options.stick_threshold = stick == "high" ? 11000 : stick == "low" ? 25000 : 18000;
  const std::string repeat = prefs_.big_screen_repeat.value_or("normal");
  options.first_repeat_ms = repeat == "fast" ? 260 : repeat == "slow" ? 520 : 380;
  options.repeat_ms = repeat == "fast" ? 60 : repeat == "slow" ? 140 : 90;
  input_->SetOptions(options);
  const int minutes = prefs_.big_screen_idle_suspend.value_or(0);
  if (minutes > 0) {
    idle_.setInterval(minutes * 60'000);
    idle_.start();
  } else {
    idle_.stop();
  }
}

void BigScreenWindow::PowerAction(const char* action) {
  if (!Power(action)) Toast("The system didn't allow that.");
}

void BigScreenWindow::RaiseFromGame() {
  showFullScreen();
  raise();
  activateWindow();
  if (QWindow* handle = windowHandle()) handle->requestActivate();
}

void BigScreenWindow::keyPressEvent(QKeyEvent* event) {
  static const std::map<int, Nav> kKeys = {
      {Qt::Key_Up, Nav::Up},          {Qt::Key_Down, Nav::Down},       {Qt::Key_Left, Nav::Left},
      {Qt::Key_Right, Nav::Right},    {Qt::Key_Return, Nav::Accept},   {Qt::Key_Enter, Nav::Accept},
      {Qt::Key_Space, Nav::Accept},   {Qt::Key_Escape, Nav::Back},     {Qt::Key_Backspace, Nav::Back},
      {Qt::Key_X, Nav::Action},       {Qt::Key_Y, Nav::Search},        {Qt::Key_Q, Nav::PrevTab},
      {Qt::Key_E, Nav::NextTab},      {Qt::Key_PageUp, Nav::PrevTab},  {Qt::Key_PageDown, Nav::NextTab},
      {Qt::Key_S, Nav::Sort},         {Qt::Key_Home, Nav::Guide},      {Qt::Key_BracketLeft, Nav::PrevLetter},
      {Qt::Key_BracketRight, Nav::NextLetter},
  };
  const auto found = kKeys.find(event->key());
  if (found == kKeys.end() || (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
    return QWidget::keyPressEvent(event);
  }
  idle_.start();
  if (found->second == Nav::Guide) return Guide();
  Navigate(found->second);
}

void BigScreenWindow::resizeEvent(QResizeEvent*) {
  for (QWidget* child : {static_cast<QWidget*>(hero_), static_cast<QWidget*>(stack_), static_cast<QWidget*>(chrome_)}) {
    child->setGeometry(rect());
  }
}

}  // namespace mira_gui::bigscreen
