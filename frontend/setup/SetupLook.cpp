#include "SetupLook.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtMath>
#include <algorithm>
#include <cmath>

#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"
#include "../library/GamePresentation.h"
#include "../settings/SettingsCard.h"
#include "../sidebar/SidebarGames.h"
#include "../sources/Sources.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "SetupWork.h"

namespace mira_gui {
namespace {

QString OfficeAppName(const QString& ref) {
  static const QHash<QString, QString> kNames = {
      {"word", "Word"},       {"excel", "Excel"},   {"powerpoint", "PowerPoint"},
      {"outlook", "Outlook"}, {"onenote", "OneNote"}, {"access", "Access"},
      {"publisher", "Publisher"},
  };
  return kNames.value(ref, ref);
}

// `picture`'s art filling `rect` with rounded corners: its hero when asked and there is one,
// else its cover, else its color with its letter.
void DrawArt(QPainter& painter, const QRectF& rect, const Picture& picture, qreal radius,
             bool hero = false) {
  painter.save();
  QPainterPath clip;
  clip.addRoundedRect(rect, radius, radius);
  painter.setClipPath(clip);
  const QPixmap art = hero && !picture.hero.isNull() ? picture.hero : picture.cover;
  if (!art.isNull()) {
    const QSizeF size = art.deviceIndependentSize();
    const qreal scale = std::max(rect.width() / size.width(), rect.height() / size.height());
    const QSizeF drawn = size * scale;
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(QRectF(rect.center().x() - drawn.width() / 2,
                              rect.center().y() - drawn.height() / 2, drawn.width(), drawn.height()),
                       art, QRectF(QPointF(0, 0), art.size()));
  } else {
    QLinearGradient fill(rect.topLeft(), rect.bottomRight());
    fill.setColorAt(0, picture.color);
    fill.setColorAt(1, picture.color.darker(hero ? 190 : 125));
    painter.fillRect(rect, fill);
    if (!picture.letter.isEmpty()) {
      QFont font = painter.font();
      font.setPixelSize(std::max(9, static_cast<int>(std::min(rect.width(), rect.height()) * 0.42)));
      font.setWeight(QFont::Bold);
      painter.setFont(font);
      painter.setPen(QColor(255, 255, 255, 225));
      painter.drawText(rect, Qt::AlignCenter, picture.letter);
    }
  }
  painter.restore();
}

// White text with a shadow, for names drawn over art.
void DrawOverArt(QPainter& painter, const QRectF& rect, const QString& text, int pixels,
                 Qt::Alignment align, const QColor& color = Qt::white, bool bold = true) {
  QFont font = painter.font();
  font.setPixelSize(pixels);
  font.setWeight(bold ? QFont::DemiBold : QFont::Normal);
  painter.setFont(font);
  const QString shown = QFontMetrics(font).elidedText(text, Qt::ElideRight, qRound(rect.width()));
  painter.setPen(QColor(0, 0, 0, 200));
  painter.drawText(rect.translated(0, 1), align, shown);
  painter.setPen(color);
  painter.drawText(rect, align, shown);
}

QPainterPath Rounded(const QRectF& rect, qreal radius) {
  QPainterPath path;
  path.addRoundedRect(rect, radius, radius);
  return path;
}

void DrawPlaceholder(QPainter& painter, const QRectF& rect, qreal radius) {
  const theme::Tokens& tokens = theme::Current();
  QPen dashed(tokens.border, 1.5, Qt::DashLine);
  painter.setPen(dashed);
  painter.setBrush(Qt::NoBrush);
  painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), radius, radius);
}

// --- Theme tiles -------------------------------------------------------------------------------

// A theme as a small window in its colors, its name under it.
class ThemeTile : public QAbstractButton {
 public:
  ThemeTile(const QString& theme, const QString& label, QWidget* parent)
      : QAbstractButton(parent), theme_(theme) {
    setText(label);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(label);
    setFixedHeight(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
  QString Theme() const { return theme_; }

 protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
    QColor fill = tokens.surface;
    if (isChecked()) {
      fill = QColor::fromRgbF(fill.redF() * 0.91 + tokens.accent.redF() * 0.09,
                              fill.greenF() * 0.91 + tokens.accent.greenF() * 0.09,
                              fill.blueF() * 0.91 + tokens.accent.blueF() * 0.09);
    }
    painter.setPen(QPen(isChecked() ? tokens.accent : tokens.border, 1.5));
    painter.setBrush(fill);
    painter.drawRoundedRect(frame, tokens.radius_panel, tokens.radius_panel);

    const QRectF mini(9, 9, width() - 18, 62);
    const auto draw = [&](const theme::Tokens& colors) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(colors.window);
      painter.drawRoundedRect(mini, 5, 5);
      painter.setBrush(colors.surface);
      painter.drawRect(QRectF(mini.left(), mini.top(), mini.width() * 0.28, mini.height()));
      const qreal left = mini.left() + mini.width() * 0.28 + 6;
      const qreal cell = (mini.right() - 6 - left - 6) / 3;
      for (int i = 0; i < 4; ++i) {
        painter.setBrush(i == 1 ? colors.accent : colors.border);
        const int column = i % 3;
        const int row = i / 3;
        painter.drawRoundedRect(
            QRectF(left + column * (cell + 3), mini.top() + 6 + row * (cell * 1.5 + 3), cell,
                   std::min(cell * 1.5, mini.height() - 12)),
            2, 2);
      }
    };
    painter.save();
    painter.setClipPath(Rounded(mini, 5));
    if (theme_ == "auto") {
      draw(theme::Peek("mira-light"));
      QPainterPath dark;
      dark.moveTo(mini.topLeft());
      dark.lineTo(mini.left() + mini.width() * 0.62, mini.top());
      dark.lineTo(mini.left() + mini.width() * 0.38, mini.bottom());
      dark.lineTo(mini.bottomLeft());
      dark.closeSubpath();
      painter.setClipPath(dark, Qt::IntersectClip);
      draw(theme::Peek("mira-dark"));
    } else {
      draw(theme::Peek(theme_));
    }
    painter.restore();
    painter.setPen(QColor(0, 0, 0, 40));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(mini, 5, 5);

    painter.setPen(tokens.text);
    painter.setFont(font());
    painter.drawText(QRectF(10, mini.bottom() + 4, width() - 20, height() - mini.bottom() - 8),
                     Qt::AlignLeft | Qt::AlignVCenter, text());
  }

 private:
  QString theme_;
};

// --- The library preview -----------------------------------------------------------------------

// Continue cards over one row of seven tiles, drawn with the applied theme and shapes.
class LibraryPreview : public QWidget {
 public:
  struct Shown {
    bool apps = false;  // only applications were picked
    bool continue_row = true;
    bool status = true;
    bool source = true;
  };

  LibraryPreview(const SetupContext& context, QWidget* parent)
      : QWidget(parent), context_(context) {
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    connect(context.art, &DemoArt::Loaded, this, qOverload<>(&QWidget::update));
    connect(context.services.artwork, &ArtworkStore::CoverChanged, this,
            qOverload<>(&QWidget::update));
    connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
      updateGeometry();  // the gap changes the tiles' size
      update();
    });
  }
  void SetShown(const Shown& shown) {
    shown_ = shown;
    // As tall as what it shows; the page under it is only space, so nothing moves.
    updateGeometry();
    update();
  }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override {
    const qreal gap = theme::Current().tile_spacing;
    const qreal tile = (width - 20 - 6 * gap) / 7;
    return qCeil(20 + (shown_.continue_row ? 40 + gap : 0) + tile * 1.5);
  }
  QSize sizeHint() const override { return {560, heightForWidth(560)}; }

 protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(tokens.border, 1));
    painter.setBrush(tokens.window);
    painter.drawRoundedRect(frame, tokens.radius_panel, tokens.radius_panel);
    painter.setClipPath(Rounded(frame, tokens.radius_panel));

    const qreal gap = tokens.tile_spacing;
    const qreal radius = tokens.radius_tile;
    const qreal pad = 10;
    const qreal inner = width() - 2 * pad;
    qreal top = pad;
    const std::vector<Picture> pictures =
        shown_.apps ? PreviewApps(context_) : PreviewGames(context_, 7, true);
    const bool examples = !shown_.apps && context_.services.library->Games().empty();

    if (shown_.continue_row) {
      const qreal card = (inner - 2 * gap) / 3;
      for (int i = 0; i < 3 && i < static_cast<int>(pictures.size()); ++i) {
        const QRectF at(pad + i * (card + gap), top, card, 40);
        DrawArt(painter, at, pictures[i], radius, /*hero=*/true);
        QLinearGradient shade(at.topLeft(), at.bottomLeft());
        shade.setColorAt(0.4, QColor(0, 0, 0, 0));
        shade.setColorAt(1, QColor(0, 0, 0, 150));
        painter.fillPath(Rounded(at, radius), shade);
        DrawOverArt(painter, at.adjusted(7, 0, -7, -4), pictures[i].name, 11,
                    Qt::AlignLeft | Qt::AlignBottom);
      }
      top += 40 + gap;
    }

    constexpr int kTiles = 7;
    const qreal tile = (inner - (kTiles - 1) * gap) / kTiles;
    for (int i = 0; i < kTiles; ++i) {
      const QRectF at(pad + i * (tile + gap), top, tile, tile * 1.5);
      if (i >= static_cast<int>(pictures.size())) {
        DrawPlaceholder(painter, at, radius);
        continue;
      }
      const Picture& picture = pictures[i];
      DrawArt(painter, at, picture, radius);
      QString line;
      QColor color = QColor(255, 255, 255, 215);
      const QString dot = QString::fromUtf8(" \xc2\xb7 ");
      if (shown_.apps) {
        if (shown_.status) line = context_.work->OfficeStarted() ? "Installing" : "Not installed";
      } else if (shown_.status && i == 1) {
        // A game playing shows how the status line looks.
        line = shown_.source && !picture.source.isEmpty() ? "Playing" + dot + picture.source : "Playing";
        color = tokens.running.lighter(150);
      } else if (shown_.source) {
        line = picture.source;
      } else if (shown_.status && i == 4) {
        line = "Needs install";
      }
      const bool name = shown_.apps || examples;
      if (line.isEmpty() && !name) continue;
      const QRectF band(at.left(), at.bottom() - 26, at.width(), 26);
      QLinearGradient shade(band.topLeft(), band.bottomLeft());
      shade.setColorAt(0, QColor(0, 0, 0, 0));
      shade.setColorAt(1, QColor(0, 0, 0, 200));
      painter.fillPath(Rounded(at, radius).intersected(Rounded(band, 0)), shade);
      if (name && !line.isEmpty()) {
        DrawOverArt(painter, band.adjusted(5, 0, -4, -12), picture.name, 10,
                    Qt::AlignLeft | Qt::AlignBottom);
      }
      DrawOverArt(painter, band.adjusted(5, 0, -4, -3), line.isEmpty() ? picture.name : line, 9,
                  Qt::AlignLeft | Qt::AlignBottom, color, line.isEmpty());
    }
  }

 private:
  const SetupContext& context_;
  Shown shown_;
};

// --- The sidebar preview -----------------------------------------------------------------------

class SidebarPreview : public QWidget {
 public:
  struct Shown {
    bool apps = false;
    QString pinned = "covers";
    QString recent = "covers";
    bool when = true;
    bool covers = true;
  };

  SidebarPreview(const SetupContext& context, QWidget* parent)
      : QWidget(parent), context_(context) {
    setFixedSize(228, 410);
    connect(context.art, &DemoArt::Loaded, this, qOverload<>(&QWidget::update));
    connect(context.services.artwork, &ArtworkStore::CoverChanged, this,
            qOverload<>(&QWidget::update));
    connect(context.services.artwork, &ArtworkStore::SlotArtChanged, this,
            qOverload<>(&QWidget::update));
    connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
            qOverload<>(&QWidget::update));
  }
  void SetShown(const Shown& shown) {
    shown_ = shown;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(tokens.border, 1));
    painter.setBrush(tokens.window);
    painter.drawRoundedRect(frame, tokens.radius_panel, tokens.radius_panel);
    // Big shelves run past the bottom, as the real sidebar would scroll.
    painter.setClipPath(Rounded(frame, tokens.radius_panel));

    const qreal left = 10;
    const qreal inner = width() - 20;
    qreal y = 8;
    const auto heading = [&](const QString& text) {
      QFont font = this->font();
      font.setPixelSize(10);
      font.setLetterSpacing(QFont::PercentageSpacing, 108);
      painter.setFont(font);
      painter.setPen(tokens.text_muted);
      painter.drawText(QRectF(left + 4, y, inner, 22), Qt::AlignLeft | Qt::AlignVCenter, text);
      y += 24;
    };
    const auto text = [&](const QRectF& at, const QString& name, const QString& below) {
      QFont font = this->font();
      font.setPixelSize(12);
      painter.setFont(font);
      painter.setPen(tokens.text);
      const QRectF top = below.isEmpty() ? at : QRectF(at.left(), at.top(), at.width(), at.height() / 2 + 2);
      painter.drawText(top, Qt::AlignLeft | (below.isEmpty() ? Qt::AlignVCenter : Qt::AlignBottom),
                       QFontMetrics(font).elidedText(name, Qt::ElideRight, qRound(at.width())));
      if (below.isEmpty()) return;
      font.setPixelSize(10);
      painter.setFont(font);
      painter.setPen(tokens.text_muted);
      painter.drawText(QRectF(at.left(), at.center().y() + 2, at.width(), at.height() / 2 - 2),
                       Qt::AlignLeft | Qt::AlignTop, below);
    };
    const auto games = [&](const std::vector<Picture>& list, const QString& style, bool when) {
      if (style == "shelf") {
        // As sidebar::Shelf lays them out: two to four across, filling the width.
        const int columns = sidebar::ShelfColumns(static_cast<int>(list.size()));
        const qreal cell = (inner - (columns - 1) * sidebar::kShelfGap) / columns;
        const qreal corner = std::min(tokens.radius_tile, 4);
        for (std::size_t i = 0; i < list.size(); ++i) {
          const QRectF cover(left + (i % columns) * (cell + sidebar::kShelfGap),
                             y + (i / columns) * (cell * 1.5 + sidebar::kShelfGap), cell,
                             cell * 1.5);
          DrawArt(painter, cover, list[i], corner);
          if (when) {
            painter.save();
            painter.setClipPath(Rounded(cover, corner), Qt::IntersectClip);
            sidebar::DrawCoverLabel(&painter, cover, list[i].short_when,
                                    tokens.font_size_small - 1);
            painter.restore();
          }
        }
        const int rows = (static_cast<int>(list.size()) + columns - 1) / columns;
        y += rows * cell * 1.5 + (rows - 1) * sidebar::kShelfGap + 6;
        return;
      }
      for (const Picture& picture : list) {
        if (style == "hero") {
          const QRectF at(left, y, inner, 44);
          DrawArt(painter, at, picture, 5, /*hero=*/true);
          QLinearGradient shade(at.topLeft(), at.bottomLeft());
          shade.setColorAt(0.3, QColor(0, 0, 0, 0));
          shade.setColorAt(1, QColor(0, 0, 0, 160));
          painter.fillPath(Rounded(at, 5), shade);
          DrawOverArt(painter, at.adjusted(8, 0, -60, -4), picture.name, 12,
                      Qt::AlignLeft | Qt::AlignBottom);
          if (when) {
            DrawOverArt(painter, at.adjusted(0, 0, -7, -5), picture.when, 10,
                        Qt::AlignRight | Qt::AlignBottom, QColor(255, 255, 255, 210), false);
          }
          y += 48;
        } else {
          DrawArt(painter, QRectF(left + 4, y + 3, 26, 39), picture, 3);
          text(QRectF(left + 39, y + 3, inner - 43, 39), picture.name, when ? picture.when : QString());
          y += 45;
        }
      }
    };

    const std::vector<Picture> recent = PreviewGames(context_, 3, true);
    std::vector<Picture> pinned;
    if (shown_.apps) {
      const std::vector<Picture> apps = PreviewApps(context_);
      pinned.assign(apps.begin(), apps.begin() + std::min<std::size_t>(2, apps.size()));
    } else {
      for (std::size_t i : {std::size_t{0}, std::size_t{2}}) {
        if (i < recent.size()) pinned.push_back(recent[i]);
      }
    }
    heading("PINNED");
    games(pinned, shown_.pinned, false);
    y += 4;

    heading("SOURCES");
    for (const auto& [name, count, pictures, color] : Sources()) {
      if (shown_.covers) {
        const QPointF base(left + 4, y + 2);
        for (int i = static_cast<int>(pictures.size()) - 1; i >= 0; --i) {
          painter.save();
          const QRectF card(0, 0, 20, 30);
          painter.translate(base + QPointF(i * 8 + 10, 15));
          painter.rotate((i - (static_cast<int>(pictures.size()) - 1) / 2.0) * 8);
          painter.translate(-10, -15);
          DrawArt(painter, card, pictures[i], 2);
          painter.restore();
        }
      } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(left + 8, y + 13, 9, 9));
      }
      text(QRectF(left + 52, y, inner - 56, 34), name, count);
      y += 38;
    }

    if (shown_.apps) return;
    y += 4;
    heading("RECENTLY PLAYED");
    games(recent, shown_.recent, shown_.when);
  }

 private:
  struct Source {
    QString name;
    QString count;
    std::vector<Picture> pictures;
    QColor color;
  };

  // The two sources with the most games, their covers fanned like the sidebar's.
  std::vector<Source> Sources() const {
    if (shown_.apps) {
      const std::vector<Picture> apps = PreviewApps(context_);
      return {{"Microsoft 365", QString("%1 apps").arg(apps.size()),
               {apps.begin(), apps.begin() + std::min<std::size_t>(3, apps.size())},
               FindSourceInfo("office")->color}};
    }
    QHash<QString, std::vector<const GameSummary*>> by_source;
    for (const GameSummary& game : context_.services.library->Games()) {
      const QString id = QString::fromStdString(SourceIdOf(game.source));
      if (FindSourceInfo(id) != nullptr) by_source[id].push_back(&game);
    }
    if (by_source.isEmpty()) {
      return {{"Steam", "3 games", PreviewGames(context_, 3), FindSourceInfo("steam")->color}};
    }
    QStringList ids = by_source.keys();
    std::ranges::sort(ids, [&by_source](const QString& a, const QString& b) {
      return by_source[a].size() > by_source[b].size();
    });
    std::vector<Source> sources;
    for (const QString& id : ids.mid(0, 2)) {
      Source source{FindSourceInfo(id)->name,
                    QString("%1 games").arg(by_source[id].size()),
                    {},
                    FindSourceInfo(id)->color};
      for (std::size_t i = 0; i < by_source[id].size() && i < 3; ++i) {
        const GameSummary& game = *by_source[id][i];
        source.pictures.push_back(
            {.cover = context_.services.artwork->Cover(game, QSize(40, 60), devicePixelRatioF()),
             .name = QString::fromStdString(game.name)});
      }
      sources.push_back(std::move(source));
    }
    return sources;
  }

  const SetupContext& context_;
  Shown shown_;
};

// Three buttons, one picked, as a segmented control; `value` names each.
QWidget* Segmented(QWidget* parent, const std::vector<std::pair<QString, QString>>& options,
                   QButtonGroup*& group) {
  auto* box = new QWidget(parent);
  box->setObjectName("segmented");
  auto* buttons = new QHBoxLayout(box);
  buttons->setContentsMargins(3, 3, 3, 3);
  buttons->setSpacing(3);
  group = new QButtonGroup(box);
  for (const auto& [value, label] : options) {
    auto* button = new QPushButton(label, box);
    button->setCheckable(true);
    button->setProperty("value", value);
    group->addButton(button);
    buttons->addWidget(button);
  }
  return box;
}

void Pick(QButtonGroup* group, const QString& value) {
  for (QAbstractButton* button : group->buttons()) {
    button->setChecked(button->property("value").toString() == value);
  }
}

QString Picked(QButtonGroup* group) {
  return group->checkedButton() != nullptr ? group->checkedButton()->property("value").toString()
                                           : QString();
}

}  // namespace

// --- Example art -----------------------------------------------------------------------------

const std::vector<DemoArt::Game>& DemoArt::Games() {
  static const std::vector<Game> kGames = {
      {"367520", "Hollow Knight", 0},
      {"2379780", "Balatro", 1},
      {"1569580", "Blue Prince", 3},
  };
  return kGames;
}

DemoArt::DemoArt(QObject* parent) : QObject(parent), network_(new QNetworkAccessManager(this)) {
  for (int i = 0; i < static_cast<int>(Games().size()); ++i) {
    for (const bool hero : {false, true}) {
      const QUrl url(QString("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/%2")
                         .arg(Games()[i].appid, hero ? "library_hero.jpg" : "library_600x900.jpg"));
      QNetworkReply* reply = network_->get(QNetworkRequest(url));
      connect(reply, &QNetworkReply::finished, this, [this, reply, i, hero] {
        reply->deleteLater();
        QPixmap art;
        if (reply->error() != QNetworkReply::NoError || !art.loadFromData(reply->readAll())) return;
        // Kept no bigger than the previews draw it.
        art = art.scaled(hero ? QSize(960, 310) : QSize(240, 360), Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
        (hero ? heroes_ : covers_).insert(i, art);
        emit Loaded();
      });
    }
  }
}

// --- What previews show ----------------------------------------------------------------------

std::vector<Picture> PreviewGames(const SetupContext& context, int count, bool heroes) {
  std::vector<const GameSummary*> games;
  for (const GameSummary& game : context.services.library->Games()) {
    const bool kept_out = std::ranges::any_of(
        game.tags, [](const std::string& tag) { return tag == "hidden" || tag == "app"; });
    if (!kept_out) games.push_back(&game);
  }
  std::ranges::stable_sort(games, std::greater{}, [](const GameSummary* game) {
    return game->last_played_at.value_or(0);
  });
  const auto samples = theme::SampleArt(theme::Current());
  std::vector<Picture> pictures;
  if (games.empty()) {
    for (int i = 0; i < static_cast<int>(DemoArt::Games().size()) && i < count; ++i) {
      const DemoArt::Game& game = DemoArt::Games()[i];
      const std::optional<std::int64_t> played =
          QDateTime::currentSecsSinceEpoch() - game.days_ago * std::int64_t{86400};
      pictures.push_back({.cover = context.art->Cover(i),
                          .hero = context.art->Hero(i),
                          .name = game.name,
                          .when = FormatPlayedAgo(played),
                          .short_when = FormatPlayedAgoShort(played),
                          .source = "Steam",
                          .color = samples[i % samples.size()],
                          .letter = game.name.left(1)});
    }
    return pictures;
  }
  ArtworkStore* artwork = context.services.artwork;
  for (int i = 0; i < static_cast<int>(games.size()) && i < count; ++i) {
    const GameSummary& game = *games[i];
    const SourceInfo* source = FindSourceInfo(QString::fromStdString(SourceIdOf(game.source)));
    pictures.push_back(
        {.cover = artwork->Cover(game, QSize(120, 180), 2.0),
         .hero = heroes ? artwork->SlotArt(game.id, "hero") : QPixmap(),
         .name = QString::fromStdString(game.name),
         .when = game.last_played_at ? FormatPlayedAgo(game.last_played_at) : QString(),
         .short_when = game.last_played_at ? FormatPlayedAgoShort(game.last_played_at) : QString(),
         .source = source != nullptr ? source->name : QString(),
         .color = artwork->CoverColor(QString::fromStdString(game.id))});
  }
  return pictures;
}

std::vector<Picture> PreviewApps(const SetupContext& context) {
  const setup::Choices& choices = *context.choices;
  QStringList refs = {"word", "excel", "powerpoint"};
  if (choices.apps && choices.office && !choices.apps_skipped && !choices.office_apps.isEmpty()) {
    refs = choices.office_apps;
  }
  std::vector<Picture> pictures;
  for (const QString& ref : refs) {
    const QString name = OfficeAppName(ref);
    pictures.push_back({.name = name, .color = OfficeAppColor(ref), .letter = name.left(1)});
  }
  return pictures;
}

QColor OfficeAppColor(const QString& ref) {
  static const QHash<QString, QColor> kColors = {
      {"word", QColor("#2b579a")},    {"excel", QColor("#217346")},
      {"powerpoint", QColor("#b7472a")}, {"outlook", QColor("#0078d4")},
      {"onenote", QColor("#7719aa")}, {"access", QColor("#a4373a")},
      {"publisher", QColor("#077568")},
  };
  return kColors.value(ref, QColor("#5c7cfa"));
}

// --- CoverFan --------------------------------------------------------------------------------

CoverFan::CoverFan(const SetupContext& context, std::function<std::vector<Picture>()> pictures,
                   QSize card, int step, qreal turn, QWidget* parent)
    : QWidget(parent), pictures_(std::move(pictures)), card_(card), step_(step), turn_(turn) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  connect(context.art, &DemoArt::Loaded, this, qOverload<>(&QWidget::update));
  connect(context.services.artwork, &ArtworkStore::CoverChanged, this,
          qOverload<>(&QWidget::update));
}

QSize CoverFan::sizeHint() const { return {card_.width() + 2 * step_ + 16, card_.height() + 12}; }

void CoverFan::paintEvent(QPaintEvent*) {
  const std::vector<Picture> pictures = pictures_();
  if (pictures.empty()) return;
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const int count = static_cast<int>(pictures.size());
  const qreal span = card_.width() + (count - 1) * step_;
  const QPointF origin((width() - span) / 2, (height() - card_.height()) / 2);
  // The first card on top, so it's drawn last.
  for (int i = count - 1; i >= 0; --i) {
    painter.save();
    painter.translate(origin + QPointF(i * step_ + card_.width() / 2.0, card_.height() / 2.0));
    painter.rotate((i - (count - 1) / 2.0) * turn_);
    const QRectF rect(-card_.width() / 2.0, -card_.height() / 2.0, card_.width(), card_.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 90));
    painter.drawRoundedRect(rect.translated(0, 3), 5, 5);
    DrawArt(painter, rect, pictures[i], 5);
    painter.restore();
  }
}

// --- Look ------------------------------------------------------------------------------------

LookPage::LookPage(const SetupContext& context) : context_(context) {
  QVBoxLayout* layout = SetupPageLayout(this, icons::Glyph::Sliders, "Look and feel",
                                        "Make it yours", "Everything here is in Settings too.");
  auto* themes = new QHBoxLayout();
  themes->setSpacing(10);
  themes_ = new QButtonGroup(this);
  for (const auto& [name, label] : {std::pair{"auto", "Follow the system"},
                                    std::pair{"mira-dark", "Dark"}, std::pair{"mira-light", "Light"}}) {
    auto* tile = new ThemeTile(name, label, this);
    themes_->addButton(tile);
    themes->addWidget(tile, /*stretch=*/1);
  }
  connect(themes_, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* button) {
    FrontendPrefs change;
    change.theme = static_cast<ThemeTile*>(button)->Theme().toStdString();
    Save(change);
  });
  layout->addLayout(themes);

  QFrame* card = SetupCard(this);
  auto* grid = new QGridLayout(card);
  grid->setContentsMargins(14, 4, 14, 4);
  grid->setHorizontalSpacing(14);
  grid->setVerticalSpacing(0);
  grid->addWidget(SetupRow(card, "Corners", "How round covers, cards and buttons are.",
                      Segmented(card, {{"square", "Square"}, {"default", "Default"}, {"round", "Round"}},
                                corners_)),
                  0, 0);
  grid->addWidget(SetupRow(card, "Spacing", "Space between covers.",
                      Segmented(card, {{"compact", "Compact"}, {"default", "Default"}, {"roomy", "Roomy"}},
                                spacing_)),
                  0, 1);
  grid->addWidget(MakeDivider(card, Qt::Horizontal), 1, 0, 1, 2);
  connect(corners_, &QButtonGroup::buttonClicked, this, [this] {
    const QString corners = Picked(corners_);
    FrontendPrefs change;
    if (corners == "default") {
      change.clear = {"tile_radius", "panel_radius", "control_radius"};
    } else {
      const bool round = corners == "round";
      change.tile_radius = round ? 12 : 2;
      change.panel_radius = round ? 14 : 2;
      change.control_radius = round ? 10 : 2;
    }
    Save(change);
  });
  connect(spacing_, &QButtonGroup::buttonClicked, this, [this] {
    const QString spacing = Picked(spacing_);
    FrontendPrefs change;
    if (spacing == "default") {
      change.clear = {"tile_spacing"};
    } else {
      change.tile_spacing = spacing == "roomy" ? 10 : 2;
    }
    Save(change);
  });

  // Games get three switches; with only applications picked, two about apps.
  games_switches_ = new QWidget(card);
  auto* games = new QHBoxLayout(games_switches_);
  games->setContentsMargins(0, 0, 0, 0);
  games->setSpacing(10);
  continue_ = new Switch(games_switches_);
  status_ = new Switch(games_switches_);
  source_ = new Switch(games_switches_);
  const auto separator = [](QWidget* parent) { return MakeDivider(parent, Qt::Vertical, 28); };
  games->addWidget(SetupRow(games_switches_, "Continue playing",
                       "Large cards for running and recently played games above the grid.", continue_),
                   1);
  games->addWidget(separator(games_switches_), 0, Qt::AlignVCenter);
  games->addWidget(SetupRow(games_switches_, "Status on tiles",
                       "Show Needs install, Broken, Playing and the like on a tile.", status_),
                   1);
  games->addWidget(separator(games_switches_), 0, Qt::AlignVCenter);
  games->addWidget(SetupRow(games_switches_, "Source on tiles",
                       "Name the store or launcher a game came from under its title.", source_),
                   1);
  grid->addWidget(games_switches_, 2, 0, 1, 2);
  apps_switches_ = new QWidget(card);
  auto* apps = new QHBoxLayout(apps_switches_);
  apps->setContentsMargins(0, 0, 0, 0);
  apps->setSpacing(10);
  recent_apps_ = new Switch(apps_switches_);
  app_status_ = new Switch(apps_switches_);
  apps->addWidget(SetupRow(apps_switches_, "Recently used apps",
                      "Large cards for the apps you used last, above the grid.", recent_apps_),
                  1);
  apps->addWidget(separator(apps_switches_), 0, Qt::AlignVCenter);
  apps->addWidget(SetupRow(apps_switches_, "Status on tiles",
                      "Show Installing, Broken, Running and the like on a tile.", app_status_),
                  1);
  grid->addWidget(apps_switches_, 2, 0, 1, 2);
  grid->setColumnStretch(0, 1);
  grid->setColumnStretch(1, 1);
  layout->addWidget(card);

  const auto saved = [this](Switch* toggle, auto field) {
    connect(toggle, &QAbstractButton::clicked, this, [this, toggle, field] {
      FrontendPrefs change;
      change.*field = toggle->isChecked();
      // Applications show in the continue row only when asked for.
      if (toggle == recent_apps_) change.library_continue_apps = toggle->isChecked();
      Save(change);
    });
  };
  saved(continue_, &FrontendPrefs::library_continue_row);
  saved(recent_apps_, &FrontendPrefs::library_continue_row);
  saved(status_, &FrontendPrefs::tile_status);
  saved(app_status_, &FrontendPrefs::tile_status);
  saved(source_, &FrontendPrefs::tile_source_mark);

  auto* preview = new LibraryPreview(context, this);
  preview_ = preview;
  layout->addWidget(preview);
  layout->addStretch(1);
}

void LookPage::Save(const FrontendPrefs& change) {
  SaveSetupPrefs(context_, change);
  Enter();
}

void LookPage::Enter() {
  const FrontendPrefs& prefs = *context_.prefs;
  const setup::Choices& choices = *context_.choices;
  const bool apps = !choices.games && choices.apps;
  const bool examples = context_.services.library->Games().empty();
  if (auto* lead = findChild<QLabel*>("setup_lead")) {
    lead->setText(QString("Everything here is in Settings too. ") +
                  (apps       ? "The preview shows your applications."
                   : examples ? "The preview uses a few example games until you have your own."
                              : "The preview is your library."));
  }
  const QString theme = QString::fromStdString(prefs.theme.value_or("auto"));
  for (QAbstractButton* button : themes_->buttons()) {
    button->setChecked(static_cast<ThemeTile*>(button)->Theme() == theme);
  }
  const int radius = prefs.tile_radius.value_or(-1);
  Pick(corners_, radius < 0 ? "default" : radius <= 3 ? "square" : radius >= 10 ? "round" : "default");
  const int spacing = prefs.tile_spacing.value_or(-1);
  Pick(spacing_, spacing < 0 ? "default" : spacing <= 3 ? "compact" : spacing >= 8 ? "roomy" : "default");
  games_switches_->setVisible(!apps);
  apps_switches_->setVisible(apps);
  continue_->setChecked(prefs.library_continue_row.value_or(true));
  recent_apps_->setChecked(prefs.library_continue_row.value_or(true) &&
                           prefs.library_continue_apps.value_or(false));
  status_->setChecked(prefs.tile_status.value_or(true));
  app_status_->setChecked(prefs.tile_status.value_or(true));
  source_->setChecked(prefs.tile_source_mark.value_or(true));
  static_cast<LibraryPreview*>(preview_)->SetShown(
      {.apps = apps,
       .continue_row = apps ? recent_apps_->isChecked() : continue_->isChecked(),
       .status = status_->isChecked(),
       .source = !apps && source_->isChecked()});
}

// --- Sidebar ---------------------------------------------------------------------------------

SidebarPage::SidebarPage(const SetupContext& context) : context_(context) {
  QVBoxLayout* layout = SetupPageLayout(
      this, icons::Glyph::Sidebar, "Look and feel", "Your sidebar",
      "Pin favorites to keep them at hand. Recently played shows the last three games you played.");
  auto* columns = new QHBoxLayout();
  columns->setSpacing(16);
  auto* left = new QVBoxLayout();
  left->setSpacing(10);
  QFrame* card = SetupCard(this);
  auto* rows = new QVBoxLayout(card);
  rows->setContentsMargins(14, 4, 14, 4);
  rows->setSpacing(0);
  const std::vector<std::pair<QString, QString>> styles = {
      {"covers", "Covers"}, {"hero", "Banners"}, {"shelf", "Shelf"}};
  rows->addWidget(SetupRow(card, "Pinned", "How pinned games draw.", Segmented(card, styles, pinned_),
                      /*below=*/true));
  games_rows_ = new QWidget(card);
  auto* game_rows = new QVBoxLayout(games_rows_);
  game_rows->setContentsMargins(0, 0, 0, 0);
  game_rows->setSpacing(0);
  game_rows->addWidget(MakeDivider(games_rows_, Qt::Horizontal));
  game_rows->addWidget(SetupRow(games_rows_, "Recently played", "How recently played games draw.",
                           Segmented(games_rows_, styles, recent_), /*below=*/true));
  game_rows->addWidget(MakeDivider(games_rows_, Qt::Horizontal));
  when_ = new Switch(games_rows_);
  game_rows->addWidget(SetupRow(games_rows_, "Say when", "When each game was last played.", when_));
  rows->addWidget(games_rows_);
  rows->addWidget(MakeDivider(card, Qt::Horizontal));
  covers_ = new Switch(card);
  rows->addWidget(SetupRow(card, "Covers on sources", "A few of each source's games beside its name.",
                      covers_));
  left->addWidget(card);
  hint_ = MakeLabel(this, QString(), "muted");
  left->addWidget(hint_);
  left->addStretch(1);
  columns->addLayout(left, /*stretch=*/1);
  auto* preview = new SidebarPreview(context, this);
  preview_ = preview;
  columns->addWidget(preview, 0, Qt::AlignTop);
  layout->addLayout(columns);
  layout->addStretch(1);

  connect(pinned_, &QButtonGroup::buttonClicked, this, [this] {
    FrontendPrefs change;
    change.sidebar_pinned_style = Picked(pinned_).toStdString();
    Save(change);
  });
  connect(recent_, &QButtonGroup::buttonClicked, this, [this] {
    FrontendPrefs change;
    change.sidebar_recent_style = Picked(recent_).toStdString();
    Save(change);
  });
  connect(when_, &QAbstractButton::clicked, this, [this](bool on) {
    FrontendPrefs change;
    change.sidebar_recent_when = on;
    Save(change);
  });
  connect(covers_, &QAbstractButton::clicked, this, [this](bool on) {
    FrontendPrefs change;
    change.sidebar_source_covers = on;
    Save(change);
  });
}

void SidebarPage::Save(const FrontendPrefs& change) {
  SaveSetupPrefs(context_, change);
  Enter();
}

void SidebarPage::Enter() {
  const FrontendPrefs& prefs = *context_.prefs;
  const setup::Choices& choices = *context_.choices;
  const bool apps = !choices.games && choices.apps;
  if (auto* lead = findChild<QLabel*>("setup_lead")) {
    lead->setText(apps ? "Pin the apps you use most to keep them one click away."
                       : "Pin favorites to keep them at hand. Recently played shows the last three "
                         "games you played.");
  }
  hint_->setText(apps ? "Pin an app from its right-click menu."
                      : "Pin a game from its right-click menu. How many recent games to show is "
                        "in Settings.");
  games_rows_->setVisible(!apps);
  const QString pinned = QString::fromStdString(prefs.sidebar_pinned_style.value_or("covers"));
  const QString recent = QString::fromStdString(prefs.sidebar_recent_style.value_or("hero"));
  Pick(pinned_, pinned);
  Pick(recent_, recent);
  when_->setChecked(prefs.sidebar_recent_when.value_or(true));
  covers_->setChecked(prefs.sidebar_source_covers.value_or(true));
  static_cast<SidebarPreview*>(preview_)->SetShown({.apps = apps,
                                                    .pinned = pinned,
                                                    .recent = recent,
                                                    .when = when_->isChecked(),
                                                    .covers = covers_->isChecked()});
}

void SidebarPage::Leave() {
  // Recently played starts with the last three as banners; Settings has the count.
  if (!context_.choices->games) return;
  FrontendPrefs change;
  if (!context_.prefs->sidebar_recent_count) change.sidebar_recent_count = 3;
  if (!context_.prefs->sidebar_recent_style) change.sidebar_recent_style = "hero";
  if (change.sidebar_recent_count || change.sidebar_recent_style) SaveSetupPrefs(context_, change);
}

}  // namespace mira_gui
