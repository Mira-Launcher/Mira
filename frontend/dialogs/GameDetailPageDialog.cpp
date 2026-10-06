#include "GameDetailPageDialog.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QLocale>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>

#include <utility>

#include "../client/api/Artwork.h"
#include "../app/ErrorHelp.h"
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

  auto* body = new QWidget(scroll);
  auto* layout = new QVBoxLayout(body);
  layout->setSpacing(12);
  scroll->setWidget(body);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  outer->addWidget(buttons);

  auto* loading = new QLabel("Loading…", body);
  loading->setProperty("role", "muted");
  layout->addWidget(loading);
  layout->addStretch(1);

  api::GetMetadataAsync(this, game_id_, [body, layout, loading](GameMetadataResult result) {
    delete loading;

    if (!result.ok || result.missing) {
      // `missing` (a 404) is the ordinary "never fetched, or fetched and
      // found nothing" case, not a failure: result.error is only set for
      // an actual transport/server failure, and is empty here otherwise.
      const QString text = result.missing ? "No store info cached for this game yet."
                                          : error_help::Describe(result.error);
      auto* empty = new QLabel(text, body);
      empty->setWordWrap(true);
      empty->setProperty("role", "muted");
      layout->insertWidget(0, empty);
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
    if (!metadata.release_date.empty()) facts << "Released " + QString::fromStdString(metadata.release_date);
    if (!metadata.developers.empty()) facts << "By " + join(metadata.developers);
    if (!metadata.genres.empty()) facts << join(metadata.genres);
    if (!metadata.price.empty()) facts << QString::fromStdString(metadata.price);
    QStringList ratings;
    if (!metadata.review_summary.empty()) {
      ratings << QString("Steam reviews: %1 (%2)")
                     .arg(QString::fromStdString(metadata.review_summary))
                     .arg(QLocale().toString(metadata.review_total));
    }
    if (metadata.metacritic_score > 0) ratings << QString("Metacritic: %1").arg(metadata.metacritic_score);
    if (!metadata.protondb_tier.empty()) {
      QString tier = QString::fromStdString(metadata.protondb_tier);
      tier[0] = tier[0].toUpper();
      ratings << "ProtonDB: " + tier;
    }
    if (!metadata.description.empty() || !facts.isEmpty() || !ratings.isEmpty() || !metadata.website.empty()) {
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
      if (!metadata.website.empty()) {
        auto* website = BodyLabel(body);
        website->setTextFormat(Qt::RichText);
        website->setOpenExternalLinks(true);
        const QString url = QString::fromStdString(metadata.website).toHtmlEscaped();
        website->setText(QString("<a href=\"%1\">%1</a>").arg(url));
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
        auto* link = BodyLabel(body);
        link->setTextFormat(Qt::RichText);
        link->setOpenExternalLinks(true);
        link->setText(QString("<a href=\"%1\">Screenshot %2</a>")
                         .arg(QString::fromStdString(url)).arg(index++));
        layout->insertWidget(row++, link);
      }
    }

    if (!metadata.trailers.empty()) {
      layout->insertWidget(row++, SectionLabel("Trailers", body));
      int index = 1;
      for (const std::string& url : metadata.trailers) {
        auto* link = BodyLabel(body);
        link->setTextFormat(Qt::RichText);
        link->setOpenExternalLinks(true);
        link->setText(QString("<a href=\"%1\">Trailer %2</a>")
                         .arg(QString::fromStdString(url)).arg(index++));
        layout->insertWidget(row++, link);
      }
    }

    if (row == 0) {
      // Art-only sources (SteamGridDB) cache none of these.
      auto* empty = new QLabel("No store info cached for this game.", body);
      empty->setWordWrap(true);
      empty->setProperty("role", "muted");
      layout->insertWidget(0, empty);
    }
  });
}

}  // namespace mira_gui
