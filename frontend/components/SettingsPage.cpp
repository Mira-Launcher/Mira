#include "SettingsPage.h"

#include <QLabel>
#include <QWidget>

#include "Card.h"
#include "ColumnGrid.h"
#include "Text.h"

namespace mira_gui {

SettingsPage::SettingsPage(const QString& title, QWidget* parent) : QWidget(parent) {
  setObjectName("settings_section");
  grid_ = new ColumnGrid(this);
  title_ = new QLabel(title, this);
  title_->setObjectName("settings_title");
  SetTextStyle(title_, text::kHeading);
  grid_->AddHeader(title_);
}

Card* SettingsPage::AddCard(const QString& title) {
  auto* card = new Card(title);
  AddWidget(card);
  return card;
}

void SettingsPage::AddWidget(QWidget* widget) {
  grid_->addWidget(widget);
}

void SettingsPage::Place(QWidget* widget, int columns, int column) {
  grid_->Place(widget, columns, column);
}

QString SettingsPage::Title() const {
  return title_->text();
}

void SettingsPage::Rearrange() {
  grid_->Reflow();
}

void SettingsPage::SetGroupHeading(const QString& text) {
  auto* heading = new QLabel(text.toUpper(), this);
  heading->setObjectName("settings_title");
  SetTextStyle(heading, text::kGroup);
  grid_->InsertHeader(0, heading);
}

int SettingsPage::GridLeft() const {
  return grid_->GridLeft(width());
}

int SettingsPage::GridWidth() const {
  return grid_->GridWidth(width());
}

}  // namespace mira_gui
