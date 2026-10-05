#include "HoverCard.h"

#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>

#include "../client/MiradClient.h"
#include "GamePresentation.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

std::string Join(const std::vector<std::string>& values) {
  std::string joined;
  for (const std::string& value : values) {
    if (!joined.empty()) joined += ", ";
    joined += value;
  }
  return joined;
}

QLabel* AddLine(QVBoxLayout* layout, QWidget* parent, const char* role) {
  auto* label = new QLabel(parent);
  label->setProperty("role", role);
  label->setWordWrap(true);
  label->hide();
  layout->addWidget(label);
  return label;
}

void SetLine(QLabel* label, const QString& text) {
  label->setText(text);
  label->setVisible(!text.isEmpty());
}

}  // namespace

namespace card {

void Paint(QWidget* widget) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(widget);
  painter.setRenderHint(QPainter::Antialiasing);
  QPainterPath path;
  path.addRoundedRect(QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), tokens.radius_panel,
                      tokens.radius_panel);
  painter.fillPath(path, tokens.surface);
  painter.setPen(QPen(tokens.border, 1));
  painter.drawPath(path);
}

QPoint Place(const QRect& anchor, QSize size, bool beside) {
  QScreen* screen = QGuiApplication::screenAt(anchor.center());
  if (screen == nullptr) screen = QGuiApplication::primaryScreen();
  return Place(anchor, size, beside, screen != nullptr ? screen->availableGeometry() : QRect(anchor.topLeft(), size));
}

QPoint Place(const QRect& anchor, QSize size, bool beside, const QRect& area) {
  QPoint pos;
  if (beside) {
    pos = QPoint(anchor.right() + 1 + kGap, anchor.top());
    if (pos.x() + size.width() > area.right()) pos.setX(anchor.left() - kGap - size.width());
  } else {
    pos = QPoint(anchor.center().x() - size.width() / 2, anchor.bottom() + 1 + kGap);
    if (pos.y() + size.height() > area.bottom()) pos.setY(anchor.top() - kGap - size.height());
  }
  pos.setX(std::clamp(pos.x(), area.left(), std::max(area.left(), area.right() + 1 - size.width())));
  pos.setY(std::clamp(pos.y(), area.top(), std::max(area.top(), area.bottom() + 1 - size.height())));
  return pos;
}

}  // namespace card

HoverDwell::HoverDwell(std::function<void(const QModelIndex&)> on_hover) : on_hover_(std::move(on_hover)) {
  timer_.setSingleShot(true);
  QObject::connect(&timer_, &QTimer::timeout, [this] {
    if (last_.isValid()) on_hover_(last_);
  });
}

void HoverDwell::Track(const QModelIndex& hovered) {
  if (QPersistentModelIndex(hovered) == last_) return;
  last_ = hovered;
  timer_.stop();
  on_hover_(QModelIndex());  // hide at once on change or leave
  if (hovered.isValid()) timer_.start(card::kDwellMs);
}

HoverCard::HoverCard(QWidget* parent) : QWidget(parent != nullptr ? parent->window() : nullptr) {
  // A child of the window, not a tooltip window of its own: a separate
  // window swallowed clicks meant for the grid beside it while it showed.
  // It never takes a click itself; what's under it does.
  setAttribute(Qt::WA_TransparentForMouseEvents);
  hide();

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(card::kPaddingX, card::kPaddingY, card::kPaddingX, card::kPaddingY);
  layout->setSpacing(4);
  setFixedWidth(240);

  name_ = new QLabel(this);
  name_->setProperty("role", "section");
  name_->setWordWrap(true);
  layout->addWidget(name_);

  status_ = new QLabel(this);
  status_->setTextFormat(Qt::RichText);
  layout->addWidget(status_);

  protondb_ = new QLabel(this);
  // Fixed, not the layout's default: a QSS-painted pill should hug its own
  // text, not stretch to the card's width like every other line here.
  protondb_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  protondb_->hide();
  layout->addWidget(protondb_);

  layout->addSpacing(4);

  platform_line_ = AddLine(layout, this, "muted");
  played_line_ = AddLine(layout, this, "muted");
  developer_line_ = AddLine(layout, this, "muted");
  error_line_ = AddLine(layout, this, "error");
  hint_line_ = AddLine(layout, this, "muted");
}

void HoverCard::Reset() {
  protondb_->hide();
  for (QLabel* line : {platform_line_, played_line_, developer_line_, error_line_, hint_line_}) {
    SetLine(line, QString());
  }
}

void HoverCard::ShowGame(const GameSummary& game, bool running, const QString& hint) {
  game_id_ = game.id;
  Reset();

  name_->setText(QString::fromStdString(game.name));
  status_->setText(QString("<span style='color:%1; font-weight:600;'>%2</span>")
                       .arg(StatusColor(running ? "running" : game.status).name(),
                            running ? "Playing" : StatusLabel(game.status)));

  const bool native = game.platform == "native";
  QString platform_label = QString::fromStdString(game.platform);
  if (!platform_label.isEmpty()) platform_label[0] = platform_label[0].toUpper();
  SetLine(platform_line_, native ? "Native, no Proton involved" : QString("Platform: %1").arg(platform_label));
  SetLine(played_line_, IsApp(game) ? FormatLastPlayed(game.last_played_at)
                                    : QString("%1 · %2").arg(FormatLastPlayed(game.last_played_at),
                                                             FormatPlaytime(game.play_seconds)));
  SetLine(error_line_, QString::fromStdString(game.last_error));
  SetLine(hint_line_, !hint.isEmpty() || !game.needs_check
                          ? hint
                          : QString("Not checked: Mira wasn't sure which program starts it. Open it to check."));
  Reposition();

  MiradClient::GetMetadataAsync(this, game.id, [this, id = game.id](GameMetadataResult result) {
    if (game_id_ != id || !result.ok || result.missing) return;
    ShowMetadata(result.metadata);
  });
}

void HoverCard::ShowTitle(const QString& title, const QString& status, const QString& detail) {
  game_id_.clear();
  Reset();
  name_->setText(title);
  status_->setText(QString("<span style='color:%1; font-weight:600;'>%2</span>")
                       .arg(theme::Current().text_muted.name(), status.toHtmlEscaped()));
  SetLine(platform_line_, detail);
  Reposition();
}

void HoverCard::ShowMetadata(const GameMetadata& metadata) {
  const QString tier = QString::fromStdString(metadata.protondb_tier);
  if (!tier.isEmpty()) {
    QString label = tier;
    label[0] = label[0].toUpper();
    const QColor background = ProtonDbTierColor(metadata.protondb_tier);
    // A real QSS-painted background, not rich-text HTML: Qt's rich text
    // engine only crudely rounds a span's background.
    protondb_->setText(label);
    protondb_->setStyleSheet(QString("QLabel { background-color: %1; color: %2; "
                                     "padding: 2px 10px; border-radius: 10px; font-weight: 700; }")
                                 .arg(background.name(), ContrastingTextColor(background).name()));
  }
  protondb_->setVisible(!tier.isEmpty());

  const std::string developer_genre = Join(metadata.developers) +
      (!metadata.developers.empty() && !metadata.genres.empty() ? " · " : "") + Join(metadata.genres);
  SetLine(developer_line_, QString::fromStdString(developer_genre));
  Reposition();
}

void HoverCard::PopUpBeside(const QRect& anchor) {
  anchor_ = anchor;
  Reposition();
  show();
  raise();
}

void HoverCard::Reposition() {
  adjustSize();
  // Placed again once metadata grows it: flipped to a tile's left, its old
  // spot would now overlap the tile. Kept inside the window it's drawn in.
  QWidget* window = parentWidget();
  if (!anchor_.isValid() || window == nullptr) return;
  const QRect area(window->mapToGlobal(QPoint(0, 0)), window->size());
  move(window->mapFromGlobal(card::Place(anchor_, size(), /*beside=*/true, area)));
}

void HoverCard::paintEvent(QPaintEvent*) { card::Paint(this); }

}  // namespace mira_gui
