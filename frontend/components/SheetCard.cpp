#include "SheetCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "../theme/Icons.h"
#include "IconButton.h"
#include "Metrics.h"
#include "Text.h"

namespace mira_gui {

SheetCard::SheetCard(const QString& title, QWidget* parent) : Card(title, parent) {}

QLabel* SheetCard::SetHeading(const QString& title, const QString& subtitle) {
  auto* heading = new QLabel(title, this);
  SetTextStyle(heading, text::kHeading);
  heading->setWordWrap(true);
  if (subtitle.isEmpty()) {
    SetTitleWidget(heading);
    return heading;
  }
  auto* lines = new QWidget(this);
  auto* layout = new QVBoxLayout(lines);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(1);
  heading->setParent(lines);
  layout->addWidget(heading);
  auto* sub = new QLabel(subtitle, lines);
  SetTextStyle(sub, text::kDetail);
  sub->setWordWrap(true);
  layout->addWidget(sub);
  SetTitleWidget(lines);
  return heading;
}

void SheetCard::SetClose(const QString& tip, const std::function<void()>& close) {
  auto* button = new IconButton(icons::Glyph::Close, tip, this);
  connect(button, &QToolButton::clicked, this, close);
  SetHeaderEnd(button);
}

QToolButton* SheetCard::SetBack(const std::function<void()>& back) {
  auto* button = new IconButton(icons::Glyph::ArrowLeft, "Back", this);
  connect(button, &QToolButton::clicked, this, back);
  SetLeading(button);
  return button;
}

QLabel* SheetCard::ErrorLine() {
  if (error_ == nullptr) {
    error_ = new QLabel(this);
    SetTextStyle(error_, text::kError);
    error_->setWordWrap(true);
    error_->setContentsMargins(kRowPaddingX, 4, kRowPaddingX, 0);
    error_->hide();
    // Right above the buttons when they're already there; else they're added after it.
    const QWidget* row = buttons_ != nullptr ? buttons_->parentWidget() : nullptr;
    auto* footer =
        row != nullptr ? qobject_cast<QBoxLayout*>(row->parentWidget()->layout()) : nullptr;
    AddFooterRow(error_, footer != nullptr ? footer->indexOf(row) : -1);
  }
  return error_;
}

QHBoxLayout* SheetCard::ButtonRow() {
  if (buttons_ == nullptr) {
    auto* row = new QWidget(this);
    buttons_ = new QHBoxLayout(row);
    buttons_->setContentsMargins(kRowPaddingX, 12, kRowPaddingX, 10);
    buttons_->setSpacing(8);
    buttons_->addStretch(1);
    AddFooterRow(row);
  }
  return buttons_;
}

QPushButton* SheetCard::AddCancel(const std::function<void()>& cancel) {
  auto* button = new QPushButton("Cancel", this);
  connect(button, &QPushButton::clicked, this, cancel);
  ButtonRow()->addWidget(button);
  return button;
}

}  // namespace mira_gui
