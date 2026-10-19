#include "GameDetailsCard.h"

#include <QDesktopServices>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTextDocumentFragment>
#include <QUrl>
#include <QVBoxLayout>

#include "../app/ErrorHelp.h"
#include "../client/api/Artwork.h"
#include "../library/GameLibraryModel.h"
#include "../library/GamePresentation.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/FlowLayout.h"
#include "../widgets/Scrolling.h"
#include "../widgets/WindowCards.h"

namespace mira_gui {
namespace {

constexpr int kCardWidth = 780;
// The hero above the store info, and the most room that info gets before it scrolls.
constexpr int kHeroHeight = 290;
constexpr int kMaxPanelHeight = 360;
constexpr QSize kShot(160, 90);
constexpr int kMaxShots = 4;
constexpr int kDescriptionLines = 4;
// The body's text width: the card less the panel's and the body's margins and a scrollbar.
constexpr int kTextWidth = kCardWidth - 40 - 48 - 12;

QString Join(const std::vector<std::string>& values) {
  QStringList list;
  for (const std::string& value : values) list << QString::fromStdString(value);
  return list.join(", ");
}

// A store-provided URL, only when it's http(s), so a store can't hand us a file:// to open.
QUrl WebUrl(const std::string& url) {
  const QUrl parsed(QString::fromStdString(url));
  return parsed.isValid() && (parsed.scheme() == "https" || parsed.scheme() == "http") ? parsed
                                                                                       : QUrl();
}

QLabel* Text(QWidget* parent, const QString& text, Qt::TextFormat format = Qt::PlainText) {
  auto* label = new QLabel(parent);
  label->setTextFormat(format);
  label->setText(text);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
  return label;
}

QString Anchor(const QString& href, const QString& text) {
  return QString("<a href=\"%1\">%2</a>").arg(href.toHtmlEscaped(), text.toHtmlEscaped());
}

// Steam's review wording, colored as it is there.
const char* ReviewState(const std::string& summary) {
  if (summary.find("Positive") != std::string::npos) return "ok";
  if (summary.find("Negative") != std::string::npos) return "error";
  return "warning";
}

const char* TierState(const std::string& tier) {
  if (tier == "platinum" || tier == "gold" || tier == "native") return "ok";
  if (tier == "borked") return "error";
  return "warning";
}

// A label that runs `clicked` on a left click: a screenshot, or the large one over the card.
class Clickable : public QLabel {
 public:
  explicit Clickable(QWidget* parent) : QLabel(parent) { setCursor(Qt::PointingHandCursor); }
  std::function<void()> clicked;

 protected:
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && clicked) clicked();
  }
};

}  // namespace

void GameDetailsCard::Open(QWidget* from, const std::string& id, GameLibraryModel* library,
                           ArtworkStore* artwork, std::function<void()> play) {
  api::GetMetadataAsync(from, id, [from, id, library, artwork, play](GameMetadataResult result) {
    cards::Show(from, new GameDetailsCard(id, result, library, artwork, play));
  });
}

GameDetailsCard::GameDetailsCard(const std::string& id, const GameMetadataResult& metadata,
                                 GameLibraryModel* library, ArtworkStore* artwork,
                                 std::function<void()> play, QWidget* parent)
    : HeroBackdrop(artwork, parent),
      id_(id),
      library_(library),
      network_(new QNetworkAccessManager(this)) {
  const GameSummary* game = library_->Find(id_);
  if (game != nullptr) ShowGame(*game);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto* top = new QHBoxLayout();
  top->setContentsMargins(14, 12, 14, 0);
  top->addStretch(1);
  // hero_action's text color in base.qss: the button is dark glass in every theme.
  auto* close = new QPushButton(this);
  close->setObjectName("hero_action");
  close->setIcon(icons::For(icons::Glyph::Close, QColor(0xe6, 0xe8, 0xec)));
  close->setToolTip("Close");
  connect(close, &QPushButton::clicked, this, [this] { cards::Close(this); });
  top->addWidget(close);
  layout->addLayout(top);
  layout->addStretch(1);

  // Name and status over the art, as the game's settings card has them.
  auto* header = new QHBoxLayout();
  header->setContentsMargins(24, 4, 20, 16);
  header->setSpacing(16);
  auto* cover = new CoverChip(artwork, this);
  if (game != nullptr) cover->ShowGame(*game);
  header->addWidget(cover, 0, Qt::AlignBottom);
  auto* identity = new QVBoxLayout();
  identity->setSpacing(4);
  identity->addStretch(1);
  title_ = new QLabel(this);
  title_->setObjectName("game_edit_title");
  title_->setWordWrap(true);
  auto* shadow = new QGraphicsDropShadowEffect(title_);
  shadow->setColor(theme::Current().surface);
  shadow->setBlurRadius(18);
  shadow->setOffset(0, 1);
  title_->setGraphicsEffect(shadow);
  identity->addWidget(title_);
  status_ = new QLabel(this);
  status_->setTextFormat(Qt::RichText);
  identity->addWidget(status_);
  header->addLayout(identity, 1);
  play_ = new QPushButton(this);
  play_->setObjectName("game_play");
  connect(play_, &QPushButton::clicked, this, std::move(play));
  header->addWidget(play_, 0, Qt::AlignBottom);
  layout->addLayout(header);

  // The store's info, on the settings card's translucent panel. Without any, the card is the
  // hero alone.
  panel_ = new QWidget(this);
  panel_->setObjectName("game_edit_panel");
  panel_->setAttribute(Qt::WA_StyledBackground);
  panel_->hide();
  const auto style_panel = [panel = panel_] {
    const theme::Tokens& current = theme::Current();
    QColor panel_color = current.window;
    panel_color.setAlphaF(0.82);
    panel->setStyleSheet(QString("QWidget#game_edit_panel { background: %1; border: 1px solid "
                                 "%2; border-radius: %3px; }")
                             .arg(theme::ColorToQss(panel_color))
                             .arg(current.border.name())
                             .arg(current.radius_panel));
  };
  style_panel();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, panel_, style_panel);
  auto* panel_layout = new QVBoxLayout(panel_);
  panel_layout->setContentsMargins(0, 0, 0, 0);
  auto* scroll = new QScrollArea(panel_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  SetUpScrolling(scroll, this);
  auto* body = new QWidget(scroll);
  body->setAutoFillBackground(false);
  body_ = new QVBoxLayout(body);
  body_->setContentsMargins(24, 18, 24, 18);
  body_->setSpacing(12);
  scroll->setWidget(body);
  scroll->viewport()->setAutoFillBackground(false);
  panel_layout->addWidget(scroll);
  auto* panel_row = new QHBoxLayout();
  panel_row->setContentsMargins(20, 0, 20, 20);
  panel_row->addWidget(panel_);
  layout->addLayout(panel_row);
  ShowMetadata(metadata);
  body_->parentWidget()->ensurePolished();
  // As tall as the info needs, so a short blurb leaves no empty panel; with More or the
  // requirements to open, the most it gets, so what opens has room.
  const int panel_width = kCardWidth - 40;
  const int panel_height =
      expandable_ ? kMaxPanelHeight
                  : std::min(body_->hasHeightForWidth() ? body_->heightForWidth(panel_width)
                                                        : body_->sizeHint().height(),
                             kMaxPanelHeight);
  panel_->setFixedHeight(panel_height);
  setFixedSize(kCardWidth, kHeroHeight + (panel_->isHidden() ? 0 : panel_height + 20));

  UpdateIdentity();
  connect(library_, &QAbstractItemModel::dataChanged, this, &GameDetailsCard::UpdateIdentity);
  connect(library_, &QAbstractItemModel::modelReset, this, &GameDetailsCard::UpdateIdentity);
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
          &GameDetailsCard::UpdateIdentity);
}

void GameDetailsCard::UpdateIdentity() {
  const GameSummary* game = library_->Find(id_);
  play_->setEnabled(game != nullptr && CanPlayOrStop(*game));
  if (game == nullptr) return;
  title_->setText(QString::fromStdString(game->name));
  const bool running = game->running;
  const SourceInfo* info = FindSourceInfo(QString::fromStdString(game->source));
  const QString source = info != nullptr ? info->name : StatusLabel(game->source);
  QStringList facts;
  if (!source.isEmpty()) facts << source;
  if (!game->platform.empty()) facts << StatusLabel(game->platform);
  if (game->play_seconds > 0 && !IsApp(*game))
    facts << FormatPlaytime(game->play_seconds) + " played";
  status_->setText(QString("<span style='color:%1; font-weight:600;'>%2</span>&nbsp;&nbsp;%3")
                       .arg(StatusColor(running ? "running" : game->status).name(),
                            running ? RunningLabel(*game) : StatusLabel(game->status),
                            facts.join(" · ").toHtmlEscaped()));
  play_->setText(running ? "Stop" : RunVerb(*game));
  // A running game's button is a red Stop, and not the default, so Enter can't end the game.
  play_->setDefault(!running);
  play_->setIcon(
      icons::For(running ? icons::Glyph::Stop : icons::Glyph::Play, theme::Current().on_accent));
  if (play_->property("stop").toBool() != running) {
    play_->setProperty("stop", running);
    play_->style()->unpolish(play_);
    play_->style()->polish(play_);
  }
}

void GameDetailsCard::ShowMetadata(const GameMetadataResult& result) {
  QWidget* body = body_->parentWidget();
  if (!result.ok && !result.missing) {
    auto* error =
        Text(body, "Could not read its store info: " + error_help::Describe(result.error));
    error->setProperty("role", "error");
    body_->addWidget(error);
    body_->addStretch(1);
    panel_->show();
    return;
  }
  if (result.missing) return;
  const GameMetadata& metadata = result.metadata;

  QStringList facts;
  if (!metadata.release_date.empty())
    facts << "Released " + QString::fromStdString(metadata.release_date).toHtmlEscaped();
  if (!metadata.developers.empty()) facts << Join(metadata.developers).toHtmlEscaped();
  if (!metadata.genres.empty()) facts << Join(metadata.genres).toHtmlEscaped();
  if (metadata.controller_support == "full") facts << "Full controller support";
  if (metadata.controller_support == "partial") facts << "Partial controller support";
  if (metadata.achievements_total > 0)
    facts << QString("%1 achievements").arg(metadata.achievements_total);
  if (!metadata.dlc_ids.empty()) facts << QString("%1 DLC").arg(metadata.dlc_ids.size());
  const bool requirements =
      !metadata.requirements_min.empty() || !metadata.requirements_rec.empty();
  if (requirements) facts << Anchor("#requirements", "System requirements");
  QLabel* requirements_text = nullptr;
  if (!facts.isEmpty()) {
    auto* line = Text(body, facts.join("&nbsp;&nbsp;·&nbsp;&nbsp;"), Qt::RichText);
    line->setProperty("role", "muted");
    body_->addWidget(line);
    if (requirements) {
      // Steam's HTML carries its own "Minimum:" / "Recommended:" headings.
      requirements_text = Text(body,
                               QString::fromStdString(metadata.requirements_min) +
                                   QString::fromStdString(metadata.requirements_rec),
                               Qt::RichText);
      requirements_text->hide();
      expandable_ = true;
      connect(line, &QLabel::linkActivated, requirements_text, [requirements_text] {
        requirements_text->setVisible(!requirements_text->isVisible());
      });
    }
  }

  if (!metadata.description.empty()) {
    // Steam's short description is plain text that may carry entities. Shown as plain text,
    // top-aligned, so the cut below lands between lines.
    auto* description =
        Text(body, QTextDocumentFragment::fromHtml(QString::fromStdString(metadata.description))
                       .toPlainText());
    description->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    body_->addWidget(description);
    // Four lines as the label lays them out, in the stylesheet's font, which only applies on
    // polish.
    description->ensurePolished();
    const int cut = description->fontMetrics()
                        .boundingRect(QRect(0, 0, kTextWidth, QWIDGETSIZE_MAX), Qt::TextWordWrap,
                                      QString("x\n").repeated(kDescriptionLines - 1) + "x")
                        .height();
    if (description->heightForWidth(kTextWidth) > cut) {
      description->setMaximumHeight(cut);
      expandable_ = true;
      auto* more = Text(body, Anchor("#more", "More"), Qt::RichText);
      body_->addWidget(more);
      connect(more, &QLabel::linkActivated, description, [description, more, cut] {
        const bool open = description->maximumHeight() == cut;
        description->setMaximumHeight(open ? QWIDGETSIZE_MAX : cut);
        more->setText(Anchor("#more", open ? "Less" : "More"));
      });
    }
  }

  std::vector<std::pair<QString, const char*>> chips;
  if (!metadata.review_summary.empty()) {
    QString review = QString::fromStdString(metadata.review_summary);
    if (metadata.review_percent >= 0) {
      review += QString(" · %1% of %2")
                    .arg(metadata.review_percent)
                    .arg(QLocale().toString(metadata.review_total));
    }
    chips.emplace_back(review, ReviewState(metadata.review_summary));
  }
  if (!metadata.protondb_tier.empty()) {
    QString tier = QString::fromStdString(metadata.protondb_tier);
    tier[0] = tier[0].toUpper();
    chips.emplace_back("ProtonDB " + tier, TierState(metadata.protondb_tier));
  }
  if (metadata.metacritic_score > 0)
    chips.emplace_back(QString("Metacritic %1").arg(metadata.metacritic_score), "");
  for (const std::string& descriptor : metadata.content_descriptors)
    chips.emplace_back(QString::fromStdString(descriptor), "");
  if (!chips.empty()) {
    auto* row = new QWidget(body);
    auto* flow = new FlowLayout(row, 6);
    flow->setContentsMargins(0, 0, 0, 0);
    for (const auto& [text, state] : chips) {
      auto* chip = new QLabel(text, row);
      chip->setObjectName("info_chip");
      chip->setProperty("state", state);
      flow->addWidget(chip);
    }
    body_->addWidget(row);
  }

  AddScreenshots(metadata.screenshots);
  if (requirements_text != nullptr) body_->addWidget(requirements_text);
  if (const QUrl website = WebUrl(metadata.website); !website.isEmpty()) {
    auto* site =
        Text(body, Anchor(website.toString(QUrl::FullyEncoded), website.host()), Qt::RichText);
    site->setOpenExternalLinks(true);
    site->setToolTip(website.toString());
    body_->addWidget(site);
  }
  if (body_->count() == 0) return;
  body_->addStretch(1);
  panel_->show();
}

void GameDetailsCard::AddScreenshots(const std::vector<std::string>& urls) {
  QWidget* body = body_->parentWidget();
  QHBoxLayout* row = nullptr;
  int shown = 0;
  for (const std::string& url : urls) {
    const QUrl web = WebUrl(url);
    if (web.isEmpty()) continue;
    if (shown++ == kMaxShots) break;
    if (row == nullptr) {
      row = new QHBoxLayout();
      row->setSpacing(8);
      body_->addLayout(row);
    }
    auto* shot = new Clickable(body);
    shot->setFixedSize(kShot);
    shot->setObjectName("screenshot");
    shot->setToolTip("Show larger");
    row->addWidget(shot);
    QNetworkReply* reply = network_->get(QNetworkRequest(web));
    connect(reply, &QNetworkReply::finished, shot, [this, reply, shot] {
      reply->deleteLater();
      QPixmap full;
      if (reply->error() != QNetworkReply::NoError || !full.loadFromData(reply->readAll())) return;
      const qreal ratio = shot->devicePixelRatioF();
      const QSize size = kShot * ratio;
      QPixmap thumb = full.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
      thumb = thumb.copy(QRect(
          QPoint((thumb.width() - size.width()) / 2, (thumb.height() - size.height()) / 2), size));
      thumb.setDevicePixelRatio(ratio);
      shot->setPixmap(thumb);
      shot->clicked = [this, full] { ShowLarge(full); };
    });
  }
  if (row != nullptr) row->addStretch(1);
}

void GameDetailsCard::ShowLarge(const QPixmap& pixmap) {
  if (large_ != nullptr) large_->deleteLater();
  auto* large = new Clickable(this);
  large->setObjectName("screenshot_large");
  large->setAttribute(Qt::WA_StyledBackground);
  large->setAlignment(Qt::AlignCenter);
  large->setGeometry(rect());
  large->setToolTip("Close");
  const qreal ratio = devicePixelRatioF();
  QPixmap scaled =
      pixmap.scaled(size() * ratio * 0.94, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  scaled.setDevicePixelRatio(ratio);
  large->setPixmap(scaled);
  large->clicked = [large] { large->deleteLater(); };
  large_ = large;
  large->show();
  large->raise();
}

}  // namespace mira_gui
