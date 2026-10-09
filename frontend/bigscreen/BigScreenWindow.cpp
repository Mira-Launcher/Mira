#include "BigScreenWindow.h"

#include <QDateTime>
#include <QKeyEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QWindow>

#include <algorithm>

#include "../client/Jobs.h"
#include "../client/api/Config.h"
#include "../client/api/Games.h"
#include "../client/api/Stores.h"
#include "../library/ArtworkStore.h"
#include "../library/GameActions.h"
#include "../library/GameLibraryModel.h"
#include "../library/GameMenus.h"
#include "../library/OwnedTitles.h"
#include "../theme/Theme.h"
#include "DownloadsPage.h"
#include "GamePage.h"
#include "GamepadInput.h"
#include "HeroBackground.h"
#include "HomePage.h"
#include "SearchPage.h"
#include "SettingsPage.h"

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
    const QString pad = !input.available()        ? "Keyboard · controllers need SDL3"
                        : input.pad_name().isEmpty() ? "No controller"
                                                     : input.pad_name();
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
      painter.setBrush(tokens.accent);
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
    if (window_->dialog_) {
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
      const double glyph_w = u * (it->nav == Nav::PrevTab || it->nav == Nav::NextTab ? 2.2 : 1.45);
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

  BigScreenWindow* window_;
};

BigScreenWindow::BigScreenWindow(LibraryServices services, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), services_(services) {
  setWindowTitle("Mira");
  setFocusPolicy(Qt::StrongFocus);
  input_ = new GamepadInput(this);
  hero_ = new HeroBackground(this);
  stack_ = new QStackedWidget(this);
  stack_->setAttribute(Qt::WA_TranslucentBackground);
  tabs_ = {new HomePage(this), new SearchPage(this), new DownloadsPage(this), new SettingsPage(this)};
  tab_names_ = {"Home", "Search", "Downloads", "Settings"};
  for (Page* page : tabs_) stack_->addWidget(page);
  game_page_ = new GamePage(this);
  stack_->addWidget(game_page_);
  for (Page* page : {tabs_[0], tabs_[1], tabs_[2], tabs_[3], static_cast<Page*>(game_page_)}) {
    connect(page, &Page::HintsChanged, this, [this] { chrome_->update(); });
  }
  chrome_ = new Chrome(this);

  connect(input_, &GamepadInput::Pressed, this, [this](Nav nav) {
    if (nav == Nav::Guide) return RaiseFromGame();
    // A game in front has the controller; only Guide comes back to Mira.
    if (!isActiveWindow()) return;
    Navigate(nav);
  });
  connect(input_, &GamepadInput::PadChanged, chrome_, qOverload<>(&QWidget::update));
  connect(services_.downloads, &DownloadTracker::Changed, chrome_, qOverload<>(&QWidget::update));
  toast_timer_.setSingleShot(true);
  toast_timer_.setInterval(2600);
  connect(&toast_timer_, &QTimer::timeout, this, [this] {
    toast_.clear();
    chrome_->update();
  });
  clock_timer_.setInterval(10'000);
  connect(&clock_timer_, &QTimer::timeout, chrome_, qOverload<>(&QWidget::update));
  clock_timer_.start();

  api::GetFrontendPrefsAsync(this, [this](FrontendPrefsResult result) {
    if (!result.ok) return;
    prefs_ = result.prefs;
    for (Page* page : tabs_) page->Shown();
    hero_->update();
    chrome_->update();
  });
  services_.owned_titles->RefreshIfStale();
  SelectTab(0);
  if (!input_->available()) Toast("Controllers need SDL3 (the sdl3 package). The keyboard works.");
}

BigScreenWindow::~BigScreenWindow() = default;

void BigScreenWindow::SetPrefs(const FrontendPrefs& prefs) {
  prefs_ = prefs;
  FrontendPrefs saved;
  saved.big_screen_buttons = prefs.big_screen_buttons;
  saved.big_screen_large_text = prefs.big_screen_large_text;
  saved.big_screen_show_uninstalled = prefs.big_screen_show_uninstalled;
  saved.big_screen_at_start = prefs.big_screen_at_start;
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
  Toast("Starting " + item.name);
  actions::Launch(this, item.game->id, [](bool) {});
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

void BigScreenWindow::closeEvent(QCloseEvent* event) {
  emit Closed();
  QWidget::closeEvent(event);
}

void BigScreenWindow::Navigate(Nav nav) {
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
    case Nav::Search: return SelectTab(1);
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
  };
  const auto found = kKeys.find(event->key());
  if (found == kKeys.end() || (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
    return QWidget::keyPressEvent(event);
  }
  Navigate(found->second);
}

void BigScreenWindow::resizeEvent(QResizeEvent*) {
  for (QWidget* child : {static_cast<QWidget*>(hero_), static_cast<QWidget*>(stack_), static_cast<QWidget*>(chrome_)}) {
    child->setGeometry(rect());
  }
}

}  // namespace mira_gui::bigscreen
