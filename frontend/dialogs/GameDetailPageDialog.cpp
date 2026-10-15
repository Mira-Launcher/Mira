#include "GameDetailPageDialog.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <utility>

#include "../app/ErrorHelp.h"
#include "../client/api/Artwork.h"
#include "../widgets/Labels.h"
#include "../widgets/Scrolling.h"

namespace mira_gui {
namespace {

QLabel* SectionLabel(const QString& text, QWidget* parent) {
  return MakeLabel(parent, text, "section", /*wrap=*/false);
}

QLabel* BodyLabel(QWidget* parent) {
  auto* label = new QLabel(parent);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

// A link to a store-provided URL, or nullptr when it isn't http(s), so a store can't hand us a
// file:// or another scheme to open.
QLabel* LinkLabel(QWidget* parent, const std::string& url, const QString& text) {
  const QUrl parsed(QString::fromStdString(url));
  if (!parsed.isValid() || (parsed.scheme() != "https" && parsed.scheme() != "http"))
    return nullptr;
  auto* label = BodyLabel(parent);
  label->setTextFormat(Qt::RichText);
  label->setOpenExternalLinks(true);
  label->setText(
      QString("<a href=\"%1\">%2</a>")
          .arg(parsed.toString(QUrl::FullyEncoded).toHtmlEscaped(), text.toHtmlEscaped()));
  return label;
}

}  // namespace

GameDetailPageDialog::GameDetailPageDialog(std::string game_id, QString game_name, QWidget* parent)
    : QDialog(parent), game_id_(std::move(game_id)) {
  setWindowTitle(game_name);
  resize(900, 700);

  auto* outer = new QVBoxLayout(this);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  SetUpScrolling(scroll, this);
  outer->addWidget(scroll, /*stretch=*/1);

  body_ = new QWidget(scroll);
  layout_ = new QVBoxLayout(body_);
  layout_->setSpacing(12);
  scroll->setWidget(body_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  outer->addWidget(buttons);

  layout_->addStretch(1);
  Load();
}

void GameDetailPageDialog::ShowEmpty(const QString& text) {
  auto* empty = new QWidget(body_);
  auto* row = new QHBoxLayout(empty);
  row->setContentsMargins(0, 0, 0, 0);
  auto* label = new QLabel(text, empty);
  label->setWordWrap(true);
  label->setProperty("role", "muted");
  row->addWidget(label, /*stretch=*/1);
  auto* fetch = new QPushButton("Fetch store info", empty);
  row->addWidget(fetch);
  layout_->insertWidget(0, empty);
  connect(fetch, &QPushButton::clicked, this, [this, empty, label, fetch] {
    fetch->setEnabled(false);
    fetch->setText("Fetching…");
    api::RefreshMetadataManyAsync(this, {game_id_},
                                  [this, empty, label, fetch](MetadataBatchResult result) {
                                    if (!result.ok) {
                                      label->setText(error_help::Describe(result.error));
                                      fetch->setEnabled(true);
                                      fetch->setText("Fetch store info");
                                      return;
                                    }
                                    fetched_ = true;
                                    delete empty;
                                    Load();
                                  });
  });
}

void GameDetailPageDialog::Load() {
  QWidget* body = body_;
  QVBoxLayout* layout = layout_;
  auto* loading = new QLabel("Loading…", body);
  loading->setProperty("role", "muted");
  layout->insertWidget(0, loading);

  api::GetMetadataAsync(this, game_id_, [this, body, layout, loading](GameMetadataResult result) {
    delete loading;

    if (!result.ok || result.missing) {
      // `missing` (a 404) is the ordinary "never fetched, or fetched and
      // found nothing" case, not a failure: result.error is only set for
      // an actual transport/server failure, and is empty here otherwise.
      if (!result.missing) {
        auto* error = new QLabel(error_help::Describe(result.error), body);
        error->setWordWrap(true);
        error->setProperty("role", "muted");
        layout->insertWidget(0, error);
        return;
      }
      ShowEmpty(fetched_ ? "No store info found for this game."
                         : "No store info cached for this game yet.");
      return;
    }

    const GameMetadata& metadata = result.metadata;
    int row = 0;
    const auto join = [](const std::vector<std::string>& values) {
      QStringList list;
      for (const std::string& value : values) list << QString::fromStdString(value);
      return list.join(", ");
    };

    // The overview: whatever of it the source had.
    QStringList facts;
    if (!metadata.release_date.empty())
      facts << "Released " + QString::fromStdString(metadata.release_date);
    if (!metadata.developers.empty()) facts << "By " + join(metadata.developers);
    if (!metadata.genres.empty()) facts << join(metadata.genres);
    if (!metadata.price.empty()) facts << QString::fromStdString(metadata.price);
    QStringList ratings;
    if (!metadata.review_summary.empty()) {
      ratings << QString("Steam reviews: %1 (%2)")
                     .arg(QString::fromStdString(metadata.review_summary))
                     .arg(QLocale().toString(metadata.review_total));
    }
    if (metadata.metacritic_score > 0)
      ratings << QString("Metacritic: %1").arg(metadata.metacritic_score);
    if (!metadata.protondb_tier.empty()) {
      QString tier = QString::fromStdString(metadata.protondb_tier);
      tier[0] = tier[0].toUpper();
      ratings << "ProtonDB: " + tier;
    }
    if (!metadata.description.empty() || !facts.isEmpty() || !ratings.isEmpty() ||
        !metadata.website.empty()) {
      layout->insertWidget(row++, SectionLabel("About", body));
      if (!facts.isEmpty()) {
        auto* facts_label = BodyLabel(body);
        facts_label->setProperty("role", "muted");
        facts_label->setText(facts.join("  ·  "));
        layout->insertWidget(row++, facts_label);
      }
      if (!metadata.description.empty()) {
        auto* description = BodyLabel(body);
        // Steam's short description is plain text that may carry entities.
        description->setTextFormat(Qt::RichText);
        description->setText(QString::fromStdString(metadata.description));
        layout->insertWidget(row++, description);
      }
      if (!ratings.isEmpty()) {
        auto* ratings_label = BodyLabel(body);
        ratings_label->setText(ratings.join("\n"));
        layout->insertWidget(row++, ratings_label);
      }
      if (auto* website =
              LinkLabel(body, metadata.website, QString::fromStdString(metadata.website))) {
        layout->insertWidget(row++, website);
      }
    }

    if (!metadata.requirements_min.empty() || !metadata.requirements_rec.empty()) {
      layout->insertWidget(row++, SectionLabel("PC requirements", body));
      // Steam's HTML carries its own "Minimum:" / "Recommended:" headings.
      for (const std::string& html : {metadata.requirements_min, metadata.requirements_rec}) {
        if (html.empty()) continue;
        auto* label = BodyLabel(body);
        label->setTextFormat(Qt::RichText);
        label->setText(QString::fromStdString(html));
        layout->insertWidget(row++, label);
      }
    }

    if (!metadata.dlc_ids.empty()) {
      layout->insertWidget(row++, SectionLabel("DLC", body));
      QStringList ids;
      for (const std::int64_t id : metadata.dlc_ids) ids << QString::number(id);
      auto* dlc_label = BodyLabel(body);
      dlc_label->setText(ids.join(", "));
      layout->insertWidget(row++, dlc_label);
    }

    if (!metadata.content_descriptors.empty()) {
      layout->insertWidget(row++, SectionLabel("Content descriptors", body));
      auto* descriptors_label = BodyLabel(body);
      QString text;
      for (const std::string& descriptor : metadata.content_descriptors) {
        text += QString("• %1\n").arg(QString::fromStdString(descriptor));
      }
      descriptors_label->setText(text.trimmed());
      layout->insertWidget(row++, descriptors_label);
    }

    if (metadata.achievements_total > 0) {
      layout->insertWidget(row++, SectionLabel("Achievements", body));
      auto* achievements_label = BodyLabel(body);
      achievements_label->setText(QString("%1 achievements").arg(metadata.achievements_total));
      layout->insertWidget(row++, achievements_label);
    }

    if (!metadata.screenshots.empty()) {
      layout->insertWidget(row++, SectionLabel("Screenshots", body));
      int index = 1;
      for (const std::string& url : metadata.screenshots) {
        if (auto* link = LinkLabel(body, url, QString("Screenshot %1").arg(index))) {
          layout->insertWidget(row++, link);
          ++index;
        }
      }
    }

    if (!metadata.trailers.empty()) {
      layout->insertWidget(row++, SectionLabel("Trailers", body));
      int index = 1;
      for (const std::string& url : metadata.trailers) {
        if (auto* link = LinkLabel(body, url, QString("Trailer %1").arg(index))) {
          layout->insertWidget(row++, link);
          ++index;
        }
      }
    }

    if (row == 0) {
      // Art-only sources (SteamGridDB) cache none of these.
      ShowEmpty(fetched_ ? "No store info found for this game."
                         : "No store info cached for this game.");
    }
  });
}

}  // namespace mira_gui
