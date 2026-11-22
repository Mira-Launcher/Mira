#include "TopBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QTimer>
#include <QToolButton>

#include "../components/IconButton.h"
#include "../components/Labels.h"
#include "../components/SpatialNav.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {

TopBar::TopBar(int min_tile, int max_tile, int tile, QWidget* parent) : TitleBar(parent) {
  spatial_nav::TabLast(this);  // window tools after the library itself

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(12, 4, 6, 4);
  layout->setSpacing(8);

  // Labels pass their clicks up, so the brand drags the window like the bar.
  auto* badge = new QLabel("M", this);
  badge->setObjectName("brand_badge");
  badge->setFixedSize(22, 22);
  badge->setAlignment(Qt::AlignCenter);
  layout->addWidget(badge);
  brand_ = new QLabel("Mira", this);
  brand_->setObjectName("brand_title");
  layout->addWidget(brand_);

  // Outside the layout, so it appears without moving anything; clicks pass to the bar.
  hidden_note_ = new QLabel(this);
  hidden_note_->setObjectName("hidden_note");
  hidden_note_->setAttribute(Qt::WA_TransparentForMouseEvents);
  hidden_note_->hide();

  layout->addStretch(1);

  notice_ = new QToolButton(this);
  notice_->setObjectName("top_notice");
  notice_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  notice_->hide();
  connect(notice_, &QToolButton::clicked, this, &TopBar::NoticeClicked);
  layout->addWidget(notice_);

  zoom_ = new QSlider(Qt::Horizontal, this);
  zoom_->setRange(min_tile, max_tile);
  zoom_->setValue(tile);
  zoom_->setMaximumWidth(120);
  zoom_->setToolTip("Tile size");
  layout->addWidget(zoom_);

  const auto tool = [this, layout](icons::Glyph glyph, const QString& tooltip) {
    auto* button = new IconButton(glyph, tooltip, this);
    layout->addWidget(button);
    return button;
  };

  big_screen_ =
      tool(icons::Glyph::Monitor, "Big screen: a fullscreen mode for a TV and a controller");
  connect(big_screen_, &QToolButton::clicked, this, &TopBar::BigScreenClicked);
  activity_ = tool(icons::Glyph::Download, "Activity");
  // Its count's text is taller than the icon; the bar shouldn't grow for it.
  activity_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
  connect(activity_, &QToolButton::clicked, this, &TopBar::ActivityClicked);
  updates_ = tool(icons::Glyph::Update, "Updates");
  updates_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
  connect(updates_, &QToolButton::clicked, this, &TopBar::UpdatesClicked);
  refresh_ = tool(icons::Glyph::Refresh, "Refresh library");
  connect(refresh_, &QToolButton::clicked, this, &TopBar::RefreshClicked);
  shortcuts_ = tool(icons::Glyph::Keyboard, "Keyboard shortcuts");
  connect(shortcuts_, &QToolButton::clicked, this, &TopBar::ShortcutsClicked);
  about_ = tool(icons::Glyph::Info, "About Mira");
  connect(about_, &QToolButton::clicked, this, &TopBar::AboutClicked);

  layout->addWidget(MakeDivider(this, Qt::Vertical, 20));

  layout->addWidget(new WindowButtons(this));
}

void TopBar::SetActivityCount(int running) {
  activity_->setText(running > 0 ? QString::number(running) : QString());
  activity_->setToolButtonStyle(running > 0 ? Qt::ToolButtonTextBesideIcon
                                            : Qt::ToolButtonIconOnly);
  activity_->setToolTip(running == 0 ? QString("Activity")
                                     : QString("Activity: %1 running").arg(running));
}

void TopBar::SetUpdatesCount(int pending) {
  updates_->setText(pending > 0 ? QString::number(pending) : QString());
  updates_->setToolButtonStyle(pending > 0 ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
  updates_->setToolTip(pending == 0 ? QString("Updates")
                                    : QString("Updates: %1 available").arg(pending));
  // The count moves the tools, and the note fits between them and the brand.
  QTimer::singleShot(0, this, &TopBar::PlaceHiddenNote);
}

QWidget* TopBar::notice_button() const {
  return notice_;
}

void TopBar::SetNotice(const QString& text, bool busy, const QString& tooltip, bool lock) {
  notice_->setText(text);
  notice_->setToolTip(tooltip);
  notice_->setIcon(lock ? icons::For(icons::Glyph::Lock, theme::Current().on_accent) : QIcon());
  theme::SetStyleProperty(notice_, "busy", busy ? "true" : "false");
  notice_->setVisible(!text.isEmpty());
  QTimer::singleShot(0, this, &TopBar::PlaceHiddenNote);
}

void TopBar::SetHiddenShortcut(const QString& shortcut) {
  hidden_note_->setText(
      shortcut.isEmpty() ? QString() : QString("Showing hidden games · %1 to leave").arg(shortcut));
  hidden_note_->adjustSize();
  PlaceHiddenNote();
}

void TopBar::PlaceHiddenNote() {
  if (hidden_note_->text().isEmpty()) {
    hidden_note_->hide();
    return;
  }
  QRect place(QPoint(0, 0), hidden_note_->sizeHint());
  place.moveCenter(rect().center());
  constexpr int kGap = 12;
  hidden_note_->setGeometry(place);
  const QWidget* tools = notice_->isVisible() ? static_cast<QWidget*>(notice_) : zoom_;
  hidden_note_->setVisible(place.left() >= brand_->geometry().right() + kGap &&
                           place.right() <= tools->geometry().left() - kGap);
}

void TopBar::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  PlaceHiddenNote();
}

}  // namespace mira_gui
