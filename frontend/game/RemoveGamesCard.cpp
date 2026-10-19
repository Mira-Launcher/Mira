#include "RemoveGamesCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <algorithm>

#include "../library/ArtworkStore.h"
#include "../sources/Sources.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(40, 60);
constexpr QSize kStackedCover(32, 48);
constexpr int kStackOverlap = 12;
constexpr int kStackMax = 3;
constexpr int kSourceBadge = 24;

// The first few games' covers, overlapping, for a card about several.
class CoverStack : public QWidget {
 public:
  CoverStack(std::vector<GameSummary> games, ArtworkStore* artwork, QWidget* parent)
      : QWidget(parent), artwork_(artwork) {
    games.resize(std::min<std::size_t>(games.size(), kStackMax));
    games_ = std::move(games);
    const int count = static_cast<int>(games_.size());
    setFixedSize(kStackedCover.width() + (count - 1) * (kStackedCover.width() - kStackOverlap),
                 kStackedCover.height());
    connect(artwork, &ArtworkStore::CoverChanged, this, [this](const QString& id) {
      for (const GameSummary& game : games_) {
        if (QString::fromStdString(game.id) == id) return update();
      }
    });
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // Last first, so the first game's cover is on top.
    for (int i = static_cast<int>(games_.size()) - 1; i >= 0; --i) {
      const QRect at(QPoint(i * (kStackedCover.width() - kStackOverlap), 0), kStackedCover);
      QPainterPath clip;
      clip.addRoundedRect(at, 4, 4);
      painter.save();
      painter.setClipPath(clip);
      painter.drawPixmap(at, artwork_->Cover(games_[i], kStackedCover, devicePixelRatioF()));
      painter.restore();
      // A ring in the card's color sets each cover off from the one under it.
      painter.setPen(QPen(palette().color(QPalette::Window), 2));
      painter.drawRoundedRect(QRectF(at).adjusted(1, 1, -1, -1), 4, 4);
    }
  }

 private:
  std::vector<GameSummary> games_;
  ArtworkStore* artwork_;
};

bool HasPrefixToDelete(const RemovalPlan& plan) {
  return !plan.prefix.empty() && plan.prefix_shared_with.empty() && !plan.folder_is_prefix;
}

}  // namespace

QString RemoveGamesCard::ActionText(const std::vector<RemovalPlan>& plans) {
  if (plans.size() != 1) return QString("Remove %1 games").arg(plans.size());
  const std::string& kind = plans.front().kind;
  if (kind == "steam") return "Uninstall in Steam";
  if (kind == "store") return "Uninstall";
  if (kind == "folder") return "Delete";
  return "Remove";
}

RemoveGamesCard::RemoveGamesCard(std::vector<GameSummary> games, std::vector<RemovalPlan> plans,
                                 ArtworkStore* artwork, QWidget* parent)
    : SettingsCard(QString(), parent), games_(std::move(games)), plans_(std::move(plans)) {
  setFixedWidth(520);
  const bool one = plans_.size() == 1;

  if (one) {
    auto* cover = new QLabel(this);
    cover->setFixedSize(kCover);
    const auto show_cover = [this, cover, artwork] {
      cover->setPixmap(artwork->Cover(games_.front(), kCover, devicePixelRatioF()));
    };
    show_cover();
    connect(artwork, &ArtworkStore::CoverChanged, cover,
            [show_cover, id = QString::fromStdString(games_.front().id)](const QString& changed) {
              if (changed == id) show_cover();
            });
    SetLeading(cover);
  } else {
    SetLeading(new CoverStack(games_, artwork, this));
  }

  const QString action = ActionText(plans_);
  SetTitle(one ? action.section(' ', 0, 0) + " " + QString::fromStdString(games_.front().name)
               : action);
  SetProminentTitle();
  // Where its files come from, as the source's logo with its name on hover.
  if (const SourceInfo* source =
          one ? FindSourceInfo(QString::fromStdString(games_.front().source)) : nullptr) {
    QLabel* badge = MakeSourceBadge(*source, kSourceBadge, this);
    badge->setToolTip(source->name);
    Header()->addWidget(badge, 0, Qt::AlignVCenter);
  }

  if (one) {
    const RemovalPlan& plan = plans_.front();
    const bool app = IsApp(games_.front());
    const QString thing = app ? "App" : "Game";
    const QString size = "%1 · %2";
    if (plan.kind == "steam") {
      AddInfoRow(thing + " files and Proton prefix", [] { return QString("Deleted by Steam"); });
    } else if (plan.kind == "store") {
      AddInfoRow(thing + " files", [this, plan, size] {
        return plan.tool.empty() ? Size(plan.id, false)
                                 : size.arg(Size(plan.id, false),
                                            "deleted by " + QString::fromStdString(plan.tool));
      });
    } else if (plan.kind == "folder") {
      const bool appimage =
          QString::fromStdString(plan.files).endsWith(".AppImage", Qt::CaseInsensitive);
      const QString label = plan.folder_is_prefix ? thing + " folder and prefix"
                            : appimage            ? QString("AppImage")
                                                  : thing + " folder";
      AddInfoRow(label, [this, plan, size] {
        const QString where = plan.files_in_prefix.empty()
                                  ? HomeRelative(QString::fromStdString(plan.files))
                                  : QString::fromStdString(plan.files_in_prefix) + " in its prefix";
        return size.arg(Size(plan.id, false), where);
      });
    } else {
      AddInfoRow("Its files", [] { return QString("Stay where they are"); });
    }
    if (!plan.prefix.empty() && !plan.prefix_shared_with.empty()) {
      AddInfoRow("Its prefix", [plan] {
        return "Kept, " + QString::fromStdString(plan.prefix_shared_with) + " uses it too";
      });
    }
  } else {
    for (const RemovalPlan& plan : plans_) {
      AddGameRow(QString::fromStdString(plan.name), [this, plan] {
        if (plan.kind == "steam") return QString("Uninstalled by Steam");
        if (plan.kind == "external") return QString("Files stay");
        const QString what = plan.kind == "store" && !plan.tool.empty()
                                 ? "uninstalled by " + QString::fromStdString(plan.tool)
                             : plan.folder_is_prefix ? QString("folder and prefix deleted")
                                                     : QString("folder deleted");
        return Size(plan.id, false) + " · " + what;
      });
    }
    if (plans_.size() > 6) SetScrollable();
  }
  AddPrefixRow();

  auto* buttons = new QWidget(this);
  auto* buttons_layout = new QHBoxLayout(buttons);
  buttons_layout->setContentsMargins(18, 12, 18, 10);
  buttons_layout->setSpacing(8);
  buttons_layout->addStretch(1);
  auto* cancel = new QPushButton("Cancel", buttons);
  connect(cancel, &QPushButton::clicked, this, &RemoveGamesCard::CloseRequested);
  buttons_layout->addWidget(cancel);
  auto* confirm = new QPushButton(action, buttons);
  // Red when it deletes anything; a plain remove only forgets the game.
  const bool deletes = std::ranges::any_of(plans_, DeletesFiles);
  if (deletes) confirm->setProperty("tone", "danger");
  connect(confirm, &QPushButton::clicked, this,
          [this] { emit Confirmed(prefix_ != nullptr && prefix_->isChecked()); });
  buttons_layout->addWidget(confirm);
  AddRow(buttons);
  // Reached by a click or the Delete key, so Return mustn't confirm it a second time.
  cancel->setFocus();
}

void RemoveGamesCard::AddInfoRow(const QString& label, std::function<QString()> text) {
  auto* row = new SettingRow(label, QString(), this);
  // One line, cut short with the whole of it as the tooltip: the size leads, so it always shows.
  auto* value = new ElidedLabel(text(), row);
  value->setProperty("role", "subtle");
  value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  row->AddControl(value, 1);
  AddRow(row);
  refresh_.push_back([value, text = std::move(text)] { value->setText(text()); });
}

void RemoveGamesCard::AddGameRow(const QString& name, std::function<QString()> text) {
  auto* row = new SettingRow(QString(), QString(), this);
  row->Label()->hide();
  auto* line = new QWidget(row);
  auto* layout = new QHBoxLayout(line);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);
  layout->addWidget(new ElidedLabel(name, line), 1);
  auto* value = MakeLabel(line, text(), "subtle");
  layout->addWidget(value);
  row->AddWideControl(line);
  AddRow(row);
  refresh_.push_back([value, text = std::move(text)] { value->setText(text()); });
}

void RemoveGamesCard::AddPrefixRow() {
  std::vector<const RemovalPlan*> with_prefix;
  for (const RemovalPlan& plan : plans_) {
    if (HasPrefixToDelete(plan)) with_prefix.push_back(&plan);
  }
  if (with_prefix.empty()) return;
  const bool one = plans_.size() == 1;
  const bool rest = one && !plans_.front().files_in_prefix.empty();
  const QString label = !one   ? "Delete their prefixes"
                        : rest ? "Delete the rest of its prefix"
                               : "Delete its prefix";
  auto* row = new SettingRow(label, QString(), this);
  auto* size = MakeLabel(row, QString(), "subtle");
  row->AddControl(size);
  prefix_ = new Switch(row);
  prefix_->setAccessibleName(label);
  row->AddControl(prefix_);
  AddRow(row);
  const auto text = [this, one, with_prefix] {
    if (!measured_) return QString("…");
    qint64 total = 0;
    for (const RemovalPlan* plan : with_prefix) {
      if (const auto it = sizes_.find(plan->id); it != sizes_.end())
        total += static_cast<qint64>(it->second.prefix);
    }
    return one ? SizeText(total) : QString("%1 · %2").arg(with_prefix.size()).arg(SizeText(total));
  };
  size->setText(text());
  refresh_.push_back([size, text] { size->setText(text()); });
}

QString RemoveGamesCard::Size(const std::string& id, bool prefix) const {
  if (!measured_) return "…";
  const auto it = sizes_.find(id);
  if (it == sizes_.end()) return "…";
  return SizeText(static_cast<qint64>(prefix ? it->second.prefix : it->second.files));
}

void RemoveGamesCard::SetSizes(const std::map<std::string, RemovalSize>& sizes) {
  sizes_ = sizes;
  measured_ = true;
  for (const auto& refresh : refresh_) refresh();
}

}  // namespace mira_gui
