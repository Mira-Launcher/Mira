#include "WindowCards.h"

#include <QEvent>
#include <QGridLayout>
#include <QPointer>
#include <QShortcut>

#include "../theme/Theme.h"
#include "ModalOverlay.h"

namespace mira_gui::cards {
namespace {

constexpr char kOverlayProperty[] = "mira_card_overlay";

// Keeps the overlay over the whole window as it resizes.
class FollowWindow : public QObject {
 public:
  FollowWindow(QWidget* window, QWidget* overlay) : QObject(overlay), overlay_(overlay) {
    window->installEventFilter(this);
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Resize)
      overlay_->setGeometry(static_cast<QWidget*>(watched)->rect());
    return false;
  }

 private:
  QWidget* overlay_;
};

}  // namespace

void Show(QWidget* from, QWidget* card) {
  QWidget* window = from != nullptr ? from->window() : nullptr;
  if (window == nullptr) {
    card->deleteLater();
    return;
  }
  if (auto* host = dynamic_cast<CardHost*>(window)) {
    if (!host->ShowCard(card)) card->deleteLater();
    return;
  }
  auto* overlay = new ModalOverlay(window);
  overlay->scrim = QColor(0, 0, 0, theme::Current().modal_scrim_alpha);
  overlay->setGeometry(window->rect());
  new FollowWindow(window, overlay);
  QPointer<QWidget> guarded(card);
  overlay->on_backdrop_clicked = [guarded] {
    if (guarded != nullptr) Close(guarded);
  };
  auto* layout = new QGridLayout(overlay);
  layout->setContentsMargins(24, 24, 24, 24);
  layout->addWidget(card, 0, 0, Qt::AlignCenter);
  card->setProperty(kOverlayProperty, QVariant::fromValue<QObject*>(overlay));
  auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), overlay);
  escape->setContext(Qt::WidgetWithChildrenShortcut);
  QObject::connect(escape, &QShortcut::activated, overlay, [guarded] {
    if (guarded != nullptr) Close(guarded);
  });
  overlay->show();
  overlay->raise();
  card->setFocus();
}

void Close(QWidget* card) {
  if (card == nullptr) return;
  if (auto* overlay = qobject_cast<QWidget*>(card->property(kOverlayProperty).value<QObject*>())) {
    overlay->deleteLater();  // the card with it
    overlay->hide();
    return;
  }
  if (auto* host = dynamic_cast<CardHost*>(card->window())) host->CloseCard(card);
}

}  // namespace mira_gui::cards
