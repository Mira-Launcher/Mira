#include "StoreInfoPage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTextDocumentFragment>
#include <QUrl>
#include <QVBoxLayout>

#include "../app/ErrorHelp.h"
#include "../client/api/Artwork.h"
#include "../components/Badge.h"
#include "../components/Card.h"
#include "../components/ColumnGrid.h"
#include "../components/FlowLayout.h"
#include "../components/Metrics.h"
#include "../components/Scrolling.h"
#include "../components/Text.h"
#include "../library/GamePresentation.h"
#include "../library/TitleRatings.h"
#include "HeroBackdrop.h"

namespace mira_gui {
namespace {

constexpr QSize kShot(160, 90);
constexpr int kMaxShots = 8;

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

// Steam's requirements HTML, set close: its own line breaks and list spacing leave a short list
// running far down the card.
QString TightRequirements(const std::string& html) {
  QString text = QString::fromStdString(html);
  text.remove(QRegularExpression("<br\\s*/?>", QRegularExpression::CaseInsensitiveOption));
  text.replace(QRegularExpression("<ul[^>]*>", QRegularExpression::CaseInsensitiveOption),
               "<ul style=\"margin-top:4px; margin-bottom:10px; margin-left:-24px; "
               "-qt-list-indent:1;\">");
  text.replace(QRegularExpression("<li[^>]*>", QRegularExpression::CaseInsensitiveOption),
               "<li style=\"margin-bottom:2px;\">");
  return text;
}

// A card row, its text from where the card's title starts.
QWidget* Row(Card* card, QWidget* content) {
  auto* row = new QWidget(card);
  auto* layout = new QVBoxLayout(row);
  layout->setContentsMargins(kLabelInset, 6, 12, 6);
  content->setParent(row);
  layout->addWidget(content);
  card->AddRow(row);
  return row;
}

// A label that runs `clicked` on a left click, Enter or Space: a screenshot, or the large one over
// the page.
class Clickable : public QLabel {
 public:
  explicit Clickable(QWidget* parent) : QLabel(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
  }
  std::function<void()> clicked;

 protected:
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && clicked) clicked();
  }
  void keyPressEvent(QKeyEvent* event) override {
    const int key = event->key();
    if (clicked && (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space))
      return clicked();
    QLabel::keyPressEvent(event);
  }
};

}  // namespace

StoreInfoPage::StoreInfoPage(std::string id, QWidget* parent)
    : QWidget(parent), id_(std::move(id)), network_(new QNetworkAccessManager(this)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SeeThrough(scroll_);
  SetUpScrolling(scroll_, this);
  auto* content = new QWidget();
  auto* content_layout = new QVBoxLayout(content);
  content_layout->setContentsMargins(kPageContentMargins);
  grid_ = new ColumnGrid(nullptr, /*max_columns=*/2);
  content_layout->addLayout(grid_);
  content_layout->addStretch(1);
  scroll_->setWidget(content);
  layout->addWidget(scroll_);

  state_ = new QLabel(content);
  state_->setWordWrap(true);
  grid_->AddHeader(state_);
  about_ = new Card("About", content);
  screenshots_ = new Card("Screenshots", content);
  requirements_ = new Card("System requirements", content);
  for (Card* card : {about_, screenshots_, requirements_}) {
    card->SetOverArt(true);
    card->hide();
    grid_->addWidget(card);
  }
  // Side by side, the description on the left and the pictures and requirements beside it.
  grid_->Place(about_, 2, 0);
  grid_->Place(screenshots_, 2, 1);
  grid_->Place(requirements_, 2, 1);
  Load();
}

void StoreInfoPage::SetColumnWidth(int width) {
  grid_->SetColumnWidth(width);
}

void StoreInfoPage::Load() {
  const int request = ++request_;
  api::GetMetadataAsync(this, id_, [this, request](GameMetadataResult result) {
    if (request == request_) Show(result);
  });
}

void StoreInfoPage::Show(const GameMetadataResult& result) {
  for (Card* card : {about_, screenshots_, requirements_}) {
    card->ClearRows();
    card->hide();
  }
  if (!result.ok && !result.missing) {
    state_->setText("Could not read its store info: " + error_help::Describe(result.error));
    SetTextStyle(state_, text::kError);
    state_->show();
    return;
  }
  const GameMetadata& metadata = result.metadata;

  QStringList facts;
  if (!metadata.release_date.empty())
    facts << "Released " + QString::fromStdString(metadata.release_date);
  if (!metadata.developers.empty()) facts << Join(metadata.developers);
  if (!metadata.genres.empty()) facts << Join(metadata.genres);
  if (metadata.controller_support == "full") facts << "Full controller support";
  if (metadata.controller_support == "partial") facts << "Partial controller support";
  if (metadata.achievements_total > 0)
    facts << QString("%1 achievements").arg(metadata.achievements_total);
  if (!metadata.dlc_ids.empty()) facts << QString("%1 DLC").arg(metadata.dlc_ids.size());
  if (!facts.isEmpty()) {
    QLabel* line = Text(nullptr, facts.join("  ·  "));
    SetTextStyle(line, text::kDetail);
    Row(about_, line);
  }
  if (!metadata.description.empty()) {
    // Steam's short description is plain text that may carry entities.
    Row(about_,
        Text(nullptr, QTextDocumentFragment::fromHtml(QString::fromStdString(metadata.description))
                          .toPlainText()));
  }

  // Reviews and the ProtonDB tier in their own colors, as Steam and ProtonDB show them.
  struct Chip {
    QString text;
    QColor color;
    Tone tone = Tone::Muted;
  };
  std::vector<Chip> chips;
  if (!metadata.review_summary.empty()) {
    QString review = QString::fromStdString(metadata.review_summary);
    if (metadata.review_percent >= 0) {
      review += QString(" · %1% of %2")
                    .arg(metadata.review_percent)
                    .arg(QLocale().toString(metadata.review_total));
    }
    chips.push_back({review, ReviewBucketColor(ReviewBucketOf(metadata.review_summary))});
  }
  if (!metadata.protondb_tier.empty()) {
    QString tier = QString::fromStdString(metadata.protondb_tier);
    tier[0] = tier[0].toUpper();
    chips.push_back({"ProtonDB " + tier, ProtonDbFilterTier(metadata.protondb_tier).empty()
                                             ? QColor()
                                             : ProtonDbTierColor(metadata.protondb_tier)});
  }
  if (!metadata.anticheat_status.empty()) {
    const std::string& status = metadata.anticheat_status;
    QString text = "Anti-cheat " + QString::fromStdString(status).toLower();
    if (!metadata.anticheat_names.empty())
      text += " · " + QString::fromStdString(metadata.anticheat_names);
    chips.push_back({text, QColor(),
                     status == "Supported" || status == "Running" ? Tone::Success
                     : status == "Denied"                         ? Tone::Danger
                                                                  : Tone::Warning});
  }
  if (metadata.metacritic_score > 0)
    chips.push_back({QString("Metacritic %1").arg(metadata.metacritic_score), QColor()});
  for (const std::string& descriptor : metadata.content_descriptors)
    chips.push_back({QString::fromStdString(descriptor), QColor()});
  if (!chips.empty()) {
    auto* holder = new QWidget();
    auto* flow = new FlowLayout(holder, 6);
    flow->setContentsMargins(0, 0, 0, 0);
    for (const Chip& chip : chips) {
      auto* badge = new Badge(chip.text, chip.tone, holder);
      badge->SetColor(chip.color);
      flow->addWidget(badge);
    }
    Row(about_, holder);
  }
  if (const QUrl website = WebUrl(metadata.website); !website.isEmpty()) {
    QLabel* site =
        Text(nullptr, Anchor(website.toString(QUrl::FullyEncoded), website.host()), Qt::RichText);
    site->setOpenExternalLinks(true);
    site->setToolTip(website.toString());
    Row(about_, site);
  }

  AddScreenshots(metadata.screenshots);
  if (!metadata.requirements_min.empty() || !metadata.requirements_rec.empty()) {
    // Steam's HTML carries its own "Minimum:" / "Recommended:" headings.
    Row(requirements_, Text(nullptr,
                            TightRequirements(metadata.requirements_min) +
                                TightRequirements(metadata.requirements_rec),
                            Qt::RichText));
  }

  bool any = false;
  for (Card* card : {about_, screenshots_, requirements_}) {
    card->setVisible(!card->Rows().isEmpty());
    any = any || !card->Rows().isEmpty();
  }
  state_->setText("No store info for this game yet.");
  SetTextStyle(state_, text::kSubtle);
  state_->setVisible(!any);
  grid_->Reflow();
}

void StoreInfoPage::AddScreenshots(const std::vector<std::string>& urls) {
  auto* holder = new QWidget();
  auto* flow = new FlowLayout(holder, 8);
  flow->setContentsMargins(0, 0, 0, 0);
  int shown = 0;
  for (const std::string& url : urls) {
    const QUrl web = WebUrl(url);
    if (web.isEmpty()) continue;
    if (shown++ == kMaxShots) break;
    auto* shot = new Clickable(holder);
    shot->setFixedSize(kShot);
    shot->setObjectName("screenshot");
    shot->setToolTip("Show larger");
    flow->addWidget(shot);
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
  if (shown == 0) {
    delete holder;
    return;
  }
  Row(screenshots_, holder);
}

void StoreInfoPage::ShowLarge(const QPixmap& pixmap) {
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

void StoreInfoPage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (large_ != nullptr) large_->setGeometry(rect());
}

}  // namespace mira_gui
