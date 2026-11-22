#include "Fold.h"

#include <QAbstractButton>
#include <QEvent>
#include <QMouseEvent>
#include <QToolButton>
#include <QWidget>

#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui::fold {

QToolButton* NewChevron(QWidget* parent, QWidget* owner, std::function<bool()> expanded,
                        std::function<void(bool)> set_expanded) {
  auto* chevron = new QToolButton(parent);
  chevron->setAutoRaise(true);
  QObject::connect(chevron, &QToolButton::clicked, owner,
                   [expanded, set_expanded] { set_expanded(!expanded()); });
  QObject::connect(theme::Notifier::Instance(), &theme::Notifier::Changed, chevron,
                   [expanded, set_expanded] { set_expanded(expanded()); });
  return chevron;
}

void ShowChevron(QToolButton* chevron, bool expanded) {
  chevron->setIcon(icons::For(expanded ? icons::Glyph::ChevronUp : icons::Glyph::ChevronDown,
                              theme::Current().text_muted));
  chevron->setToolTip(expanded ? "Fold" : "Show");
}

bool FoldClick(QWidget* head, QEvent* event) {
  if (event->type() != QEvent::MouseButtonRelease) return false;
  const auto* mouse = static_cast<QMouseEvent*>(event);
  for (QWidget* hit = head->childAt(mouse->position().toPoint()); hit != nullptr && hit != head;
       hit = hit->parentWidget()) {
    if (qobject_cast<QAbstractButton*>(hit) != nullptr) return false;
  }
  return true;
}

}  // namespace mira_gui::fold
