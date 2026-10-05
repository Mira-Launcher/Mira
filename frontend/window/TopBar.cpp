#include "TopBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSlider>
#include <QToolButton>
#include <QWindow>

#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {

TopBar::TopBar(int min_tile, int max_tile, int tile, QWidget* parent) : QWidget(parent) {
  setObjectName("top_bar");

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(12, 4, 6, 4);
  layout->setSpacing(8);

  // Labels pass their clicks up, so the brand drags the window like the bar.
  auto* badge = new QLabel("M", this);
  badge->setObjectName("brand_badge");
  badge->setFixedSize(22, 22);
  badge->setAlignment(Qt::AlignCenter);
  layout->addWidget(badge);
  auto* title = new QLabel("Mira", this);
  title->setObjectName("brand_title");
  layout->addWidget(title);

  layout->addStretch(1);

  zoom_ = new QSlider(Qt::Horizontal, this);
  zoom_->setRange(min_tile, max_tile);
  zoom_->setValue(tile);
  zoom_->setMaximumWidth(120);
  zoom_->setToolTip("Tile size");
  layout->addWidget(zoom_);

  const auto tool = [this, layout](const QString& tooltip) {
    auto* button = new QToolButton(this);
    button->setAutoRaise(true);
    button->setToolTip(tooltip);
    layout->addWidget(button);
    return button;
  };

  activity_ = tool("Activity");
  // Its count's text is taller than the icon; the bar shouldn't grow for it.
  activity_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
  connect(activity_, &QToolButton::clicked, this, &TopBar::ActivityClicked);
  refresh_ = tool("Refresh library");
  connect(refresh_, &QToolButton::clicked, this, &TopBar::RefreshClicked);
  shortcuts_ = tool("Keyboard shortcuts");
  connect(shortcuts_, &QToolButton::clicked, this, &TopBar::ShortcutsClicked);
  about_ = tool("About Mira");
  connect(about_, &QToolButton::clicked, this, &TopBar::AboutClicked);

  divider_ = new QWidget(this);
  divider_->setFixedSize(1, 20);
  layout->addWidget(divider_);

  minimize_ = tool("Minimize");
  connect(minimize_, &QToolButton::clicked, this, [this] { window()->showMinimized(); });
  maximize_ = tool("Maximize");
  connect(maximize_, &QToolButton::clicked, this, &TopBar::ToggleMaximize);
  close_ = tool("Close");
  close_->setObjectName("close_button");
  connect(close_, &QToolButton::clicked, this, [this] { window()->close(); });

  ApplyIcons();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, &TopBar::ApplyIcons);
}

void TopBar::SetActivityCount(int running) {
  activity_->setText(running > 0 ? QString::number(running) : QString());
  activity_->setToolButtonStyle(running > 0 ? Qt::ToolButtonTextBesideIcon
                                            : Qt::ToolButtonIconOnly);
  activity_->setToolTip(running == 0 ? QString("Activity")
                                     : QString("Activity: %1 running").arg(running));
}

void TopBar::SyncMaximized() {
  const bool maximized = window()->isMaximized();
  maximize_->setIcon(icons::For(maximized ? icons::Glyph::Restore : icons::Glyph::Maximize));
  maximize_->setToolTip(maximized ? "Restore" : "Maximize");
}

void TopBar::ApplyIcons() {
  using icons::Glyph;
  refresh_->setIcon(icons::For(Glyph::Refresh));
  activity_->setIcon(icons::For(Glyph::Download));
  shortcuts_->setIcon(icons::For(Glyph::Keyboard));
  about_->setIcon(icons::For(Glyph::Info));
  divider_->setStyleSheet(QString("background: %1;").arg(theme::Current().border.name()));
  minimize_->setIcon(icons::For(Glyph::Minimize));
  close_->setIcon(icons::For(Glyph::Close));
  SyncMaximized();
}

void TopBar::ToggleMaximize() {
  if (window()->isMaximized()) {
    window()->showNormal();
  } else {
    window()->showMaximized();
  }
}

// The bar's own background, plus labels on it (a label passes its clicks
// up); a click on a control goes to it instead.
void TopBar::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && window()->windowHandle() != nullptr) {
    window()->windowHandle()->startSystemMove();
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void TopBar::mouseDoubleClickEvent(QMouseEvent* event) {
  ToggleMaximize();
  event->accept();
}

}  // namespace mira_gui
