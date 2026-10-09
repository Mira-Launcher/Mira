#include "GamePage.h"

#include <QPainter>

#include <algorithm>

#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"
#include "../library/GamePresentation.h"
#include "../library/OwnedTitles.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {
namespace {

constexpr double kMargin = 2.6, kTop = 6.4, kCoverW = 17, kCoverH = 25.5;

}  // namespace

GamePage::GamePage(BigScreenWindow* window) : Page(window) {
  const LibraryServices& services = window->services();
  connect(services.library, &GameLibraryModel::Changed, this, &GamePage::Refresh);
  connect(services.owned_titles, &OwnedTitles::Changed, this, &GamePage::Refresh);
  connect(services.downloads, &DownloadTracker::Changed, this, &GamePage::Refresh);
  connect(services.artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
}

void GamePage::Open(const Item& item, Page* from) {
  item_ = item;
  if (from != this) from_ = from;
  focus_ = 0;
}

void GamePage::Shown() {
  window_->ShowHero(item_);
  Refresh();
}

void GamePage::Refresh() {
  // A title installed becomes the game "<source>-<ref>", the key it already has.
  if (Item fresh = window_->Find(item_.key); !fresh.key.isEmpty()) item_ = std::move(fresh);
  focus_ = std::clamp(focus_, 0, int(Buttons().size()) - 1);
  update();
  emit HintsChanged();
}

std::vector<GamePage::Button> GamePage::Buttons() const {
  std::vector<Button> buttons;
  const DownloadTracker::Entry* download = window_->Download(item_);
  if (download != nullptr && download->state == DownloadTracker::State::Paused) {
    buttons = {{Action::Resume, "Resume"}, {Action::Cancel, "Cancel install"}};
  } else if (download != nullptr) {
    if (DownloadTracker::CanPause(*download)) buttons.push_back({Action::Pause, "Pause"});
    buttons.push_back({Action::Cancel, "Cancel install"});
  } else if (!item_.game) {
    buttons = {{Action::Install, "Install"}};
  } else if (item_.game->running) {
    buttons = {{Action::Stop, "Stop"}};
  } else if (item_.game->status == "needs_install") {
    buttons = {{Action::Install, "Install"}};
  } else {
    buttons = {{Action::Play, "Play"}};
  }
  if (item_.game) {
    buttons.push_back({Action::Pin, IsPinned(*item_.game) ? "Unpin" : "Pin"});
    if (download == nullptr && !item_.game->running) buttons.push_back({Action::Uninstall, "Uninstall"});
  }
  return buttons;
}

bool GamePage::Navigate(Nav nav) {
  const std::vector<Button> buttons = Buttons();
  switch (nav) {
    case Nav::Left:
    case Nav::Right:
      focus_ = std::clamp(focus_ + (nav == Nav::Right ? 1 : -1), 0, int(buttons.size()) - 1);
      update();
      return true;
    case Nav::Up:
    case Nav::Down: return true;
    case Nav::Action: window_->QuickAction(item_); return true;
    case Nav::Accept: break;
    default: return false;
  }
  switch (buttons[size_t(focus_)].action) {
    case Action::Play: window_->Play(item_); break;
    case Action::Stop: window_->Stop(item_); break;
    case Action::Install: window_->Install(item_); break;
    case Action::Pause: window_->Pause(item_); break;
    case Action::Resume: window_->Resume(item_); break;
    case Action::Cancel: window_->CancelInstall(item_); break;
    case Action::Pin: window_->TogglePin(item_); break;
    case Action::Uninstall: window_->Uninstall(item_); break;
  }
  return true;
}

QList<Hint> GamePage::Hints() const { return {{Nav::Accept, "Select"}, {Nav::Back, "Back"}}; }

void GamePage::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  const QSize cover_size(qRound(kCoverW * u), qRound(kCoverH * u));
  const double progress = window_->Progress(item_);
  DrawCover(painter, QRectF(QPointF(kMargin * u, kTop * u), cover_size), window_->Cover(item_, cover_size), u, false,
            false, -1);

  const double left = (kMargin + kCoverW + kMargin) * u;
  const double w = width() - left - kMargin * u;
  painter.setPen(tokens.text);
  painter.setFont(Font(u, 3.0, QFont::ExtraBold));
  const QRectF title_box(left, kTop * u, w, 7 * u);
  const QRectF title_used = painter.boundingRect(title_box, Qt::TextWordWrap, item_.name);
  painter.drawText(title_box, Qt::TextWordWrap, item_.name);
  double y = std::min(title_used.bottom(), title_box.bottom()) + u * 0.9;

  const DownloadTracker::Entry* download = window_->Download(item_);
  const bool paused = download && download->state == DownloadTracker::State::Paused;
  double x = left;
  x += DrawPill(painter, {x, y}, u, StatusText(item_, progress, paused), StatusColor(item_, progress)) + u * 0.5;
  DrawPill(painter, {x, y}, u, SourceName(item_.source));
  y += u * 3.2;

  // Buttons.
  painter.setFont(Font(u, 1.05, QFont::Bold));
  x = left;
  const std::vector<Button> buttons = Buttons();
  for (size_t i = 0; i < buttons.size(); ++i) {
    const bool primary = i == 0, focused = int(i) == focus_;
    const bool danger = buttons[i].action == Action::Uninstall || buttons[i].action == Action::Cancel;
    const double bw = painter.fontMetrics().horizontalAdvance(buttons[i].label) + u * 2.8;
    QRectF box(x, y, bw, u * 2.8);
    if (focused) box = QRectF(box.center() - QPointF(bw, box.height()) * 0.525, box.size() * 1.05);
    painter.setPen(focused ? QPen(tokens.text, u * 0.12) : Qt::NoPen);
    painter.setBrush(focused && danger ? tokens.error : primary ? tokens.accent : QColor(255, 255, 255, 26));
    painter.drawRoundedRect(box, u * 0.45, u * 0.45);
    painter.setPen(primary ? tokens.on_accent : danger && !focused ? tokens.error.lighter(140) : tokens.text);
    painter.drawText(box, Qt::AlignCenter, buttons[i].label);
    x += bw + u * 0.8;
  }
  y += u * 4.2;

  if (download != nullptr) {
    painter.setFont(Font(u, 0.9));
    painter.setPen(tokens.text_muted);
    const QString text = paused ? StatusText(item_, progress, true) : DownloadTracker::ProgressText(*download);
    painter.drawText(QPointF(left, y), text);
    const QRectF bar(left, y + u * 0.7, std::min(w, 34 * u), u * 0.45);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 28));
    painter.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
    painter.setBrush(paused ? tokens.status_missing : tokens.accent);
    painter.drawRoundedRect(QRectF(bar.topLeft(), QSizeF(bar.width() * std::max(0.0, progress), bar.height())),
                            bar.height() / 2, bar.height() / 2);
    y += u * 3;
  }

  // Facts, as label over value.
  QList<std::pair<QString, QString>> facts;
  if (item_.game) {
    const GameSummary& game = *item_.game;
    facts.append({"Played", game.play_seconds > 0 ? FormatPlaytime(game.play_seconds) : QString("—")});
    facts.append({"Last played", FormatLastPlayed(game.last_played_at)});
    facts.append({"Runs with", game.platform == "native"      ? QString("Linux build")
                               : game.runner_ref.empty() ? QString("Default runner")
                                                         : QString::fromStdString(game.runner_ref)});
  } else {
    facts.append({"Store", SourceName(item_.source)});
  }
  x = left;
  for (const auto& [label, value] : facts) {
    painter.setFont(Font(u, 0.8, QFont::DemiBold));
    painter.setPen(tokens.text_muted);
    painter.drawText(QPointF(x, y), label.toUpper());
    painter.setFont(Font(u, 1.05, QFont::DemiBold));
    painter.setPen(tokens.text);
    painter.drawText(QPointF(x, y + u * 1.6), value);
    x += std::max(painter.fontMetrics().horizontalAdvance(value), int(u * 8)) + u * 2.4;
  }
}

}  // namespace mira_gui::bigscreen
