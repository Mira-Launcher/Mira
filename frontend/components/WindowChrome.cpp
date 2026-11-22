#include "WindowChrome.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QWindow>

#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "IconButton.h"

namespace mira_gui {

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
  setObjectName("top_bar");
  setAttribute(Qt::WA_StyledBackground);  // a plain QWidget subclass draws no QSS background
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && window()->windowHandle() != nullptr) {
    window()->windowHandle()->startSystemMove();
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
  ToggleMaximized(window());
  event->accept();
}

WindowButtons::WindowButtons(QWidget* parent) : QWidget(parent) {
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  auto* minimize = new IconButton(icons::Glyph::Minimize, "Minimize", this);
  connect(minimize, &QToolButton::clicked, this, [this] { window()->showMinimized(); });
  layout->addWidget(minimize);
  maximize_ = new QToolButton(this);
  maximize_->setAutoRaise(true);
  connect(maximize_, &QToolButton::clicked, this, [this] { ToggleMaximized(window()); });
  layout->addWidget(maximize_);
  auto* close = new IconButton(icons::Glyph::Close, "Close", this);
  close->setObjectName("close_button");
  connect(close, &QToolButton::clicked, this, [this] { window()->close(); });
  layout->addWidget(close);
  Sync();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, &WindowButtons::Sync);
}

// The window is known for sure only once shown here: the bar may be built before it's placed.
void WindowButtons::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (watched_ == window()) return;
  if (watched_ != nullptr) watched_->removeEventFilter(this);
  watched_ = window();
  watched_->installEventFilter(this);
  Sync();
}

bool WindowButtons::eventFilter(QObject* watched, QEvent* event) {
  if (watched == watched_ && event->type() == QEvent::WindowStateChange) Sync();
  return QWidget::eventFilter(watched, event);
}

void WindowButtons::Sync() {
  const bool maximized = window()->isMaximized();
  const QString tip = maximized ? "Restore" : "Maximize";
  maximize_->setIcon(icons::For(maximized ? icons::Glyph::Restore : icons::Glyph::Maximize));
  maximize_->setToolTip(tip);
  maximize_->setAccessibleName(tip);
}

void ToggleMaximized(QWidget* window) {
  window->isMaximized() ? window->showNormal() : window->showMaximized();
}

}  // namespace mira_gui
