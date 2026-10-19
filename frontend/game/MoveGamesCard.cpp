#include "MoveGamesCard.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "../app/ErrorHelp.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/WindowCards.h"
#include "CardParts.h"

namespace mira_gui {
namespace {

using Part = RelocationPlan::Part;

constexpr int kWidth = 560;

bool PartMoves(const Part& part) {
  return part.action == "move" || part.action == "follow";
}

bool AnyCopies(const RelocationPlan& plan) {
  return (plan.files.action == "move" && plan.files.copies) ||
         (plan.prefix.action == "move" && plan.prefix.copies);
}

QString Parent(const std::string& path) {
  const QString text = QString::fromStdString(path);
  return HomeRelative(text.section('/', 0, -2));
}

// What happens to one part, in a line under its label.
QString PartLine(const Part& part, bool files) {
  if (part.action == "move") {
    QString line = HomeRelative(QString::fromStdString(part.from)) + "  →  " +
                   HomeRelative(QString::fromStdString(part.to));
    if (part.copies) line += "\nCopied to the other drive, then removed from " + Parent(part.from);
    return line;
  }
  if (part.action == "follow") {
    return files ? QString("Inside its prefix, so they move with it")
                 : QString("Around the game files, so it moves with them");
  }
  if (part.action == "stay") return "Stays: " + QString::fromStdString(part.reason);
  if (part.action == "in_place") {
    const std::string& at = part.to.empty() ? part.from : part.to;
    return "Already in " + Parent(at);
  }
  return {};
}

// For a game of several: what moves, in a few words.
QString Summary(const RelocationPlan& plan) {
  QStringList moving;
  if (PartMoves(plan.files)) moving << "Game files";
  if (PartMoves(plan.prefix)) moving << (moving.isEmpty() ? "Prefix" : "prefix");
  QString text = moving.join(" and ");
  if (plan.files.action == "follow") text = "Prefix, with the game files inside it";
  if (plan.prefix.action == "follow") text = "Game files, with the prefix around them";
  if (AnyCopies(plan)) text += ", to another drive";
  return text;
}

// Why a game with nothing to move stays, shared by every game with the same reason.
QString Unmoved(const RelocationPlan& plan) {
  if (!plan.ok) return "Can't move: " + error_help::Describe(plan.error);
  for (const Part* part : {&plan.files, &plan.prefix}) {
    if (part->action == "stay") return "Nothing: " + QString::fromStdString(part->reason);
  }
  return "Already in Mira's folders";
}

QLabel* Line(QWidget* parent, const QString& text, const char* role) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", role);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

// Opens or closes a game's lines on a click on its row, or Right and Left on it.
class Toggle : public QObject {
 public:
  Toggle(QWidget* row, std::function<void(std::optional<bool>)> toggle)
      : QObject(row), toggle_(std::move(toggle)) {
    row->installEventFilter(this);
    row->setFocusPolicy(Qt::StrongFocus);
  }

 protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::MouseButtonRelease) {
      toggle_(std::nullopt);
      return true;
    }
    if (event->type() == QEvent::KeyPress) {
      const int key = static_cast<QKeyEvent*>(event)->key();
      if (key == Qt::Key_Right || key == Qt::Key_Left) {
        toggle_(key == Qt::Key_Right);
        return true;
      }
    }
    return false;
  }

 private:
  std::function<void(std::optional<bool>)> toggle_;
};

}  // namespace

bool MoveGamesCard::Moves(const RelocationPlan& plan) {
  return plan.ok && (PartMoves(plan.files) || PartMoves(plan.prefix));
}

MoveGamesCard::MoveGamesCard(std::vector<RelocationPlan> plans, QWidget* parent)
    : SettingsCard(QString(), parent), plans_(std::move(plans)) {
  setFixedWidth(kWidth);
  SetProminentTitle();
  std::vector<std::string> moving;
  for (const RelocationPlan& plan : plans_) {
    if (Moves(plan)) moving.push_back(plan.id);
  }
  const bool one = plans_.size() == 1;
  AddCardHeading(this,
                 one ? "Move " + QString::fromStdString(plans_.front().name)
                     : QString("Move %1 games").arg(plans_.size()),
                 "Cancel", [this] { cards::Close(this); });

  if (one) {
    const RelocationPlan& plan = plans_.front();
    if (!plan.ok) {
      auto* row = new SettingRow("Can't move it", QString(), this);
      row->SetBelow(Line(row, error_help::Describe(plan.error), "error"));
      AddRow(row);
    }
    for (const auto& [part, files] : {std::pair{&plan.files, true}, {&plan.prefix, false}}) {
      if (!plan.ok || part->action == "none") continue;
      auto* row = new SettingRow(files ? "Game files" : "Prefix", QString(), this);
      row->SetBelow(Line(row, PartLine(*part, files), part->copies ? "warning" : "muted"));
      if (part->action == "move") {
        auto* size = Line(row, QString(), part->copies ? "warning" : "subtle");
        size->setWordWrap(false);
        row->AddControl(size);
        refresh_.push_back([this, size, id = plan.id, files, copies = part->copies] {
          const auto it = sizes_.find(id);
          const QString amount =
              !measured_ || it == sizes_.end()
                  ? QString("…")
                  : SizeText(static_cast<qint64>(files ? it->second.files : it->second.prefix));
          size->setText(amount + (copies ? " · another drive" : " · same drive"));
        });
      }
      AddRow(row);
    }
  } else {
    // Games with nothing to move, grouped by why.
    std::vector<std::pair<QString, QStringList>> unmoved;
    for (const RelocationPlan& plan : plans_) {
      if (Moves(plan)) {
        auto* row = new SettingRow(QString::fromStdString(plan.name), QString(), this);
        auto* chevron = new QToolButton(row);
        chevron->setAutoRaise(true);
        chevron->setToolTip("Show what moves");
        row->SetLeading(chevron);
        auto* below = new QWidget(row);
        auto* below_layout = new QVBoxLayout(below);
        below_layout->setContentsMargins(0, 0, 0, 0);
        below_layout->setSpacing(6);
        // What moves, in a word, until the row opens to the lines that say it in full.
        QLabel* summary = Line(below, Summary(plan), "muted");
        below_layout->addWidget(summary);
        auto* detail = new QWidget(below);
        auto* detail_layout = new QVBoxLayout(detail);
        detail_layout->setContentsMargins(0, 4, 0, 0);
        detail_layout->setSpacing(6);
        AddPartLines(detail, plan);
        detail->hide();
        below_layout->addWidget(detail);
        row->SetBelow(below);
        auto* size = Line(row, QString(), AnyCopies(plan) ? "warning" : "subtle");
        size->setWordWrap(false);
        row->AddControl(size);
        refresh_.push_back([this, size, plan] { size->setText(MovingSize(plan)); });
        // isVisibleTo: the card may not be shown yet.
        const auto chevron_icon = [chevron, detail, row] {
          chevron->setIcon(icons::For(detail->isVisibleTo(row) ? icons::Glyph::ChevronUp
                                                               : icons::Glyph::ChevronDown));
        };
        chevron_icon();
        connect(theme::Notifier::Instance(), &theme::Notifier::Changed, chevron, chevron_icon);
        const auto toggle = [detail, summary, row, chevron_icon](std::optional<bool> open) {
          detail->setVisible(open.value_or(!detail->isVisibleTo(row)));
          summary->setVisible(!detail->isVisibleTo(row));
          chevron_icon();
        };
        connect(chevron, &QToolButton::clicked, row, [toggle] { toggle(std::nullopt); });
        new Toggle(row, toggle);
        AddRow(row);
        continue;
      }
      const QString why = Unmoved(plan);
      auto it = std::ranges::find(unmoved, why, &std::pair<QString, QStringList>::first);
      if (it == unmoved.end()) it = unmoved.insert(unmoved.end(), {why, {}});
      it->second << QString::fromStdString(plan.name);
    }
    for (const auto& [why, names] : unmoved) {
      auto* row = new SettingRow(QString(), QString(), this);
      row->Label()->hide();
      auto* lines = new QWidget(row);
      auto* layout = new QVBoxLayout(lines);
      layout->setContentsMargins(0, 0, 0, 0);
      layout->setSpacing(1);
      auto* list = new ElidedLabel(names.join(", "), lines);
      list->setToolTip(names.join("\n"));
      layout->addWidget(list);
      layout->addWidget(Line(lines, why, why.startsWith("Can't") ? "error" : "muted"));
      row->AddWideControl(lines);
      AddRow(row);
    }
    if (static_cast<int>(moving.size() + unmoved.size()) > 6) SetScrollable();
  }

  QHBoxLayout* buttons = AddButtonRow(this);
  if (!one) {
    auto* total = Line(this, QString(), "subtle");
    total->setWordWrap(false);
    buttons->insertWidget(0, total);
    refresh_.push_back([this, total] {
      if (!measured_) return total->setText("…");
      qint64 sum = 0;
      for (const RelocationPlan& plan : plans_) {
        const auto it = sizes_.find(plan.id);
        if (!Moves(plan) || it == sizes_.end()) continue;
        if (plan.files.action == "move") sum += static_cast<qint64>(it->second.files);
        if (plan.prefix.action == "move") sum += static_cast<qint64>(it->second.prefix);
      }
      total->setText(SizeText(sum));
    });
  }
  auto* cancel = new QPushButton("Cancel", this);
  connect(cancel, &QPushButton::clicked, this, [this] { cards::Close(this); });
  buttons->addWidget(cancel);
  auto* move = new QPushButton(
      moving.size() <= 1 || one ? QString("Move") : QString("Move %1 games").arg(moving.size()),
      this);
  move->setDefault(true);
  move->setEnabled(!moving.empty());
  connect(move, &QPushButton::clicked, this, [this, moving] { emit Confirmed(moving); });
  buttons->addWidget(move);
  for (const auto& refresh : refresh_) refresh();
}

void MoveGamesCard::AddPartLines(QWidget* parent, const RelocationPlan& plan) {
  for (const auto& [part, files] : {std::pair{&plan.files, true}, {&plan.prefix, false}}) {
    if (part->action == "none") continue;
    parent->layout()->addWidget(Line(parent, files ? "Game files" : "Prefix", "subtle"));
    parent->layout()->addWidget(
        Line(parent, PartLine(*part, files), part->copies ? "warning" : "muted"));
  }
}

QString MoveGamesCard::MovingSize(const RelocationPlan& plan) const {
  const auto it = sizes_.find(plan.id);
  if (!measured_ || it == sizes_.end()) return "…";
  qint64 sum = 0;
  if (plan.files.action == "move") sum += static_cast<qint64>(it->second.files);
  if (plan.prefix.action == "move") sum += static_cast<qint64>(it->second.prefix);
  return SizeText(sum);
}

void MoveGamesCard::SetSizes(const std::map<std::string, RemovalSize>& sizes) {
  sizes_ = sizes;
  measured_ = true;
  for (const auto& refresh : refresh_) refresh();
}

}  // namespace mira_gui
