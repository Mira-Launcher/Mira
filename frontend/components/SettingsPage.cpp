#include "SettingsPage.h"

#include <QLabel>
#include <QLayout>
#include <QWidget>
#include <algorithm>

#include "Card.h"
#include "Labels.h"
#include "Metrics.h"

namespace mira_gui {

namespace {

// A page's heading, title and cards stacked in one column centered in the page, and a preview
// beside it while there's room (the space either side of the column, less a margin).
class CenteredColumn : public QLayout {
 public:
  explicit CenteredColumn(QWidget* parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }
  ~CenteredColumn() override {
    while (QLayoutItem* item = takeAt(0)) delete item;
  }

  void addItem(QLayoutItem* item) override {
    items_.append(item);
    invalidate();
  }
  void InsertWidget(int index, QWidget* widget) {
    addChildWidget(widget);
    items_.insert(std::clamp(index, 0, static_cast<int>(items_.size())), new QWidgetItem(widget));
    invalidate();
  }
  int count() const override { return static_cast<int>(items_.size()); }
  QLayoutItem* itemAt(int index) const override { return items_.value(index); }
  QLayoutItem* takeAt(int index) override {
    if (index < 0 || index >= items_.size()) return nullptr;
    return items_.takeAt(index);
  }
  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return Place(QRect(0, 0, width, 0), false); }
  QSize sizeHint() const override { return {kWideColumn, heightForWidth(kWideColumn)}; }
  QSize minimumSize() const override {
    int width = 0;
    for (const QLayoutItem* item : items_)
      if (!item->isEmpty()) width = std::max(width, item->minimumSize().width());
    return {std::min(width, 480), 0};
  }
  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    Place(rect, true);
  }
  int ColumnLeft(int width) const { return (width - SettingsColumnWidth(width)) / 2; }

 private:
  static int HeightOf(const QLayoutItem* item, int width) {
    return item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height();
  }

  int Place(const QRect& rect, bool apply) const {
    const int column = SettingsColumnWidth(rect.width());
    const int left = rect.x() + ColumnLeft(rect.width());
    int y = rect.y();
    bool first = true;
    bool after_title = false;
    for (QLayoutItem* item : items_) {
      if (item->isEmpty()) continue;
      if (!first) y += after_title ? kTitleGap : kCardGap;
      const int height = HeightOf(item, column);
      if (apply) item->setGeometry(QRect(left, y, column, height));
      y += height;
      first = false;
      after_title = item->widget() != nullptr && item->widget()->objectName() == "settings_title";
    }
    return y - rect.y();
  }

  static constexpr int kCardGap = 16;

  QList<QLayoutItem*> items_;
};

}  // namespace

SettingsPage::SettingsPage(const QString& title, QWidget* parent) : QWidget(parent) {
  setObjectName("settings_section");
  auto* layout = new CenteredColumn(this);
  title_ = new QLabel(title, this);
  title_->setObjectName("settings_title");
  SetTextStyle(title_, text::kHeading);
  layout->addWidget(title_);
}

Card* SettingsPage::AddCard(const QString& title) {
  auto* card = new Card(title);
  AddWidget(card);
  return card;
}

void SettingsPage::AddWidget(QWidget* widget) {
  layout()->addWidget(widget);
}

QString SettingsPage::Title() const {
  return title_->text();
}

void SettingsPage::Rearrange() {
  layout()->invalidate();
}

void SettingsPage::SetGroupHeading(const QString& text) {
  auto* heading = new QLabel(text.toUpper(), this);
  heading->setObjectName("settings_title");
  heading->setProperty("role", "nav_heading");
  static_cast<CenteredColumn*>(layout())->InsertWidget(0, heading);
}

int SettingsPage::ColumnLeft() const {
  return static_cast<CenteredColumn*>(layout())->ColumnLeft(width());
}

}  // namespace mira_gui
