#include "GamePage.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocumentFragment>
#include <QTextLayout>

#include <algorithm>

#include "../client/api/Artwork.h"
#include "../client/api/Games.h"
#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"
#include "../library/GamePresentation.h"
#include "../library/OwnedTitles.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"
#include "Screenshots.h"

namespace mira_gui::bigscreen {
namespace {

constexpr double kMargin = 2.6, kTop = 6.4, kCoverW = 17, kCoverH = 25.5, kLogoH = 6;
constexpr double kShotW = 14, kShotGap = 0.8;
constexpr qsizetype kMaxShotBytes = 8 * 1024 * 1024;
constexpr qsizetype kMaxOwnShots = 6;
constexpr int kOwnShotPx = 640;

std::optional<QColor> ProtonColor(const QString& tier) {
  static const QHash<QString, QString> colors = {{"platinum", "#b4c7dc"}, {"gold", "#cfb53b"},
                                                 {"silver", "#a6a6a6"},   {"bronze", "#cd7f32"},
                                                 {"borked", "#ff4d4d"}};
  const auto it = colors.constFind(tier);
  if (it == colors.constEnd()) return std::nullopt;
  return QColor(*it);
}

// Word-wraps `text` to at most `max_lines`, eliding the last one. Returns its height.
double DrawParagraph(QPainter& painter, QPointF top_left, double width, const QString& text, int max_lines) {
  const QFontMetricsF metrics(painter.font());
  QTextLayout layout(text, painter.font());
  layout.beginLayout();
  QList<int> starts;
  for (QTextLine line = layout.createLine(); line.isValid() && int(starts.size()) <= max_lines;
       line = layout.createLine()) {
    line.setLineWidth(width);
    starts.append(line.textStart());
  }
  layout.endLayout();
  const bool more = int(starts.size()) > max_lines;
  const int count = more ? max_lines : int(starts.size());
  for (int i = 0; i < count; ++i) {
    const int end = i + 1 < int(starts.size()) ? starts[i + 1] : text.size();
    QString line = text.mid(starts[i], end - starts[i]).trimmed();
    if (more && i == count - 1) line = metrics.elidedText(text.mid(starts[i]), Qt::ElideRight, width);
    painter.drawText(QPointF(top_left.x(), top_left.y() + i * metrics.height() + metrics.ascent()), line);
  }
  return count * metrics.height();
}

void DrawShot(QPainter& painter, const QRectF& rect, const QPixmap& shot, double unit) {
  painter.save();
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  QPainterPath clip;
  clip.addRoundedRect(rect, unit * 0.4, unit * 0.4);
  painter.setClipPath(clip);
  QRectF target(QPointF(0, 0), QSizeF(shot.size()).scaled(rect.size(), Qt::KeepAspectRatioByExpanding));
  target.moveCenter(rect.center());
  painter.drawPixmap(target, shot, QRectF(shot.rect()));
  painter.restore();
}

}  // namespace

GamePage::GamePage(BigScreenWindow* window) : Page(window), network_(new QNetworkAccessManager(this)) {
  const LibraryServices& services = window->services();
  connect(services.library, &GameLibraryModel::Changed, this, &GamePage::Refresh);
  connect(services.owned_titles, &OwnedTitles::Changed, this, &GamePage::Refresh);
  connect(services.downloads, &DownloadTracker::Changed, this, &GamePage::Refresh);
  connect(services.artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
  connect(services.artwork, &ArtworkStore::RequestsDropped, this, qOverload<>(&QWidget::update));
}

void GamePage::Open(const Item& item, Page* from) {
  item_ = item;
  if (from != this) from_ = from;
  focus_ = 0;
  description_.clear();
  proton_tier_.clear();
  controller_.clear();
  metadata_loaded_ = false;
  last_session_.reset();
  for (QPixmap& shot : shots_) shot = QPixmap();
  own_shots_.clear();
  if (item.game) {
    LoadOwnShots();
    LoadDetails(item.key);
  }
}

void GamePage::LoadDetails(const QString& key) {
  api::GetMetadataAsync(this, key.toStdString(), [this, key](GameMetadataResult result) {
    if (key != item_.key || !result.ok) return;
    const GameMetadata& meta = result.metadata;
    metadata_loaded_ = true;
    controller_ = meta.controller_support;
    description_ = QTextDocumentFragment::fromHtml(QString::fromStdString(meta.description)).toPlainText().simplified();
    proton_tier_ = QString::fromStdString(meta.protondb_tier).toLower();
    FetchShots(key, meta.screenshots);
    update();
  });
  api::GetGameSessionsAsync(this, key.toStdString(), 1, [this, key](GameSessionsResult result) {
    if (key != item_.key || !result.ok || result.sessions.empty()) return;
    last_session_ = result.sessions.front();
    update();
  });
}

void GamePage::FetchShots(const QString& key, const std::vector<std::string>& urls) {
  for (size_t i = 0; i < std::min(shots_.size(), urls.size()); ++i) {
    QNetworkReply* reply = network_->get(QNetworkRequest(QUrl(QString::fromStdString(urls[i]))));
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, i] {
      reply->deleteLater();
      const QByteArray data = reply->readAll();
      if (reply->error() != QNetworkReply::NoError || key != item_.key || data.size() > kMaxShotBytes) return;
      QPixmap pixmap;
      if (!pixmap.loadFromData(data)) return;
      shots_[i] = pixmap;
      update();
    });
  }
}

void GamePage::LoadOwnShots() {
  own_shots_.clear();
  for (const QString& path : ScreenshotsFor(item_.name)) {
    if (own_shots_.size() == kMaxOwnShots) break;
    QPixmap shot(path);
    if (shot.isNull()) continue;
    if (shot.width() > kOwnShotPx) shot = shot.scaledToWidth(kOwnShotPx, Qt::SmoothTransformation);
    own_shots_.push_back(shot);
  }
}

void GamePage::Shown() {
  window_->ShowHero(item_);
  if (item_.game) LoadOwnShots();
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
  const QRectF title_box(left, kTop * u, w, 7 * u);
  QRectF title_used = title_box;
  const QPixmap logo = window_->Logo(item_);
  if (logo.isNull()) {
    title_used = DrawTitle(painter, title_box, u, 3.0, item_.name);
  } else {
    const QSizeF logo_size = QSizeF(logo.size()).scaled(QSizeF(w, kLogoH * u), Qt::KeepAspectRatio);
    const QRectF logo_rect(QPointF(left, kTop * u + (kLogoH * u - logo_size.height()) / 2), logo_size);
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(logo_rect, logo, QRectF(logo.rect()));
    painter.restore();
  }
  double y = title_used.bottom() + u * 0.9;

  const DownloadTracker::Entry* download = window_->Download(item_);
  const bool paused = download && download->state == DownloadTracker::State::Paused;
  double x = left;
  x += DrawPill(painter, {x, y}, u, StatusText(item_, progress, paused), StatusColor(item_, progress)) + u * 0.5;
  x += DrawPill(painter, {x, y}, u, SourceName(item_.source)) + u * 0.5;
  if (!proton_tier_.isEmpty()) {
    const QString label = "ProtonDB " + proton_tier_.left(1).toUpper() + proton_tier_.mid(1);
    DrawPill(painter, {x, y}, u, label, ProtonColor(proton_tier_));
  }
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
    painter.setBrush(focused && danger ? tokens.error : primary ? Accent() : QColor(255, 255, 255, 26));
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
    painter.setBrush(paused ? tokens.status_missing : Accent());
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
    if (last_session_) facts.append({"Last session", FormatPlaytime(last_session_->duration_seconds)});
    if (!controller_.empty()) {
      facts.append({"Controller", controller_ == "full" ? QString("Full support") : QString("Partial support")});
    } else if (metadata_loaded_ && item_.source == "steam") {
      facts.append({"Controller", QString("Keyboard and mouse")});
    }
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

  // Description, then up to three screenshots, as far as the page has room.
  y += u * 4.0;
  if (!description_.isEmpty()) {
    painter.setFont(Font(u, 0.95));
    painter.setPen(tokens.text_muted);
    y += DrawParagraph(painter, {left, y}, w, description_, 5) + u * 1.2;
  }
  const QSizeF shot_size(kShotW * u, kShotW * u * 9 / 16);
  if (!own_shots_.empty() && y + u * 1.6 + shot_size.height() <= height()) {
    painter.setFont(Font(u, 0.8, QFont::DemiBold));
    painter.setPen(tokens.text_muted);
    painter.drawText(QPointF(left, y + u * 1.2), "Your screenshots");
    y += u * 1.6;
    double own_x = left;
    for (const QPixmap& shot : own_shots_) {
      DrawShot(painter, QRectF(QPointF(own_x, y), shot_size), shot, u);
      own_x += shot_size.width() + kShotGap * u;
    }
    y += shot_size.height() + u * 1.2;
  }
  double shot_x = left;
  for (const QPixmap& shot : shots_) {
    if (shot.isNull()) continue;
    if (y + shot_size.height() > height()) break;
    DrawShot(painter, QRectF(QPointF(shot_x, y), shot_size), shot, u);
    shot_x += shot_size.width() + kShotGap * u;
  }
}

}  // namespace mira_gui::bigscreen
