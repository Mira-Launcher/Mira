#include "SetupWindow.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "../app/Appearance.h"
#include "../client/api/Config.h"
#include "../library/GameLibraryModel.h"
#include "../sources/Sources.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/Scrolling.h"
#include "SetupLook.h"
#include "SetupPages.h"
#include "SetupWork.h"

namespace mira_gui {

// The pages as dots, the current one drawn long.
class StepDots : public QWidget {
 public:
  explicit StepDots(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
  void Set(int count, int current) {
    count_ = count;
    current_ = current;
    update();
  }
  QSize sizeHint() const override { return {count_ * 13 + 12, 12}; }

 protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const int width = (count_ - 1) * 13 + 20;
    qreal x = (this->width() - width) / 2.0;
    for (int i = 0; i < count_; ++i) {
      const bool on = i == current_;
      QColor color = on ? tokens.accent : tokens.border;
      if (i < current_) color = QColor::fromRgbF((tokens.accent.redF() + tokens.border.redF()) / 2,
                                                 (tokens.accent.greenF() + tokens.border.greenF()) / 2,
                                                 (tokens.accent.blueF() + tokens.border.blueF()) / 2);
      painter.setBrush(color);
      const qreal w = on ? 20 : 7;
      painter.drawRoundedRect(QRectF(x, (height() - 7) / 2.0, w, 7), 3.5, 3.5);
      x += w + 6;
    }
  }

 private:
  int count_ = 0;
  int current_ = 0;
};

QVBoxLayout* SetupPageLayout(SetupStep* page, icons::Glyph glyph, const QString& eyebrow,
                             const QString& title, const QString& lead) {
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(36, 14, 36, 22);
  layout->setSpacing(12);
  auto* head = new QHBoxLayout();
  head->setSpacing(14);
  auto* badge = new QLabel(page);
  badge->setObjectName("setup_badge");
  badge->setFixedSize(44, 44);
  badge->setAlignment(Qt::AlignCenter);
  badge->setPixmap(icons::For(glyph, theme::Current().accent).pixmap(QSize(24, 24), page->devicePixelRatioF()));
  head->addWidget(badge, 0, Qt::AlignTop);
  auto* text = new QVBoxLayout();
  text->setSpacing(2);
  auto* small = new QLabel(eyebrow.toUpper(), page);
  small->setProperty("role", "group_heading");
  text->addWidget(small);
  auto* heading = new QLabel(title, page);
  heading->setObjectName("setup_title");
  heading->setWordWrap(true);
  text->addWidget(heading);
  auto* lead_label = MakeLabel(page, lead, "muted");
  lead_label->setObjectName("setup_lead");
  text->addWidget(lead_label);
  head->addLayout(text, /*stretch=*/1);
  layout->addLayout(head);
  layout->addSpacing(4);
  return layout;
}

QWidget* SetupLaterLine(QWidget* parent, const QString& text) {
  auto* row = new QWidget(parent);
  auto* line = new QHBoxLayout(row);
  line->setContentsMargins(0, 4, 0, 0);
  line->setSpacing(8);
  auto* clock = new QLabel(row);
  clock->setPixmap(icons::For(icons::Glyph::Clock, theme::Current().text_muted)
                       .pixmap(QSize(15, 15), row->devicePixelRatioF()));
  line->addWidget(clock, 0, Qt::AlignTop);
  auto* label = MakeLabel(row, text, "muted");
  label->setTextFormat(Qt::RichText);
  line->addWidget(label, /*stretch=*/1);
  return row;
}

QFrame* SetupCard(QWidget* parent) {
  auto* card = new QFrame(parent);
  card->setObjectName("setup_card");
  return card;
}

QWidget* SetupRow(QWidget* parent, const QString& label, const QString& doc, QWidget* control,
                  bool below, QWidget* after) {
  auto* row = new QWidget(parent);
  auto* column = new QVBoxLayout(row);
  column->setContentsMargins(0, 6, 0, 6);
  column->setSpacing(6);
  auto* line = new QHBoxLayout();
  line->setSpacing(8);
  auto* name = new QLabel(label, row);
  name->setMinimumHeight(26);
  name->setToolTip(doc);
  name->setAccessibleDescription(doc);
  line->addWidget(name);
  if (after != nullptr) line->addWidget(after);
  line->addStretch(1);
  if (!below) line->addWidget(control);
  column->addLayout(line);
  if (below) column->addWidget(control);
  control->setAccessibleName(label);
  return row;
}

void SaveSetupPrefs(const SetupContext& context, const FrontendPrefs& change) {
  FrontendPrefs& prefs = *context.prefs;
  const auto merge = [](auto& into, const auto& from) {
    if (from) into = from;
  };
  merge(prefs.theme, change.theme);
  merge(prefs.tile_radius, change.tile_radius);
  merge(prefs.panel_radius, change.panel_radius);
  merge(prefs.control_radius, change.control_radius);
  merge(prefs.tile_spacing, change.tile_spacing);
  merge(prefs.library_continue_row, change.library_continue_row);
  merge(prefs.library_continue_apps, change.library_continue_apps);
  merge(prefs.library_apps_in_all, change.library_apps_in_all);
  merge(prefs.tile_status, change.tile_status);
  merge(prefs.tile_source_mark, change.tile_source_mark);
  merge(prefs.sidebar_pinned_style, change.sidebar_pinned_style);
  merge(prefs.sidebar_recent_style, change.sidebar_recent_style);
  merge(prefs.sidebar_recent_when, change.sidebar_recent_when);
  merge(prefs.sidebar_recent_count, change.sidebar_recent_count);
  merge(prefs.sidebar_source_covers, change.sidebar_source_covers);
  merge(prefs.big_screen_show_apps, change.big_screen_show_apps);
  merge(prefs.big_screen_at_start, change.big_screen_at_start);
  merge(prefs.start_on_login, change.start_on_login);
  for (const std::string& key : change.clear) {
    if (key == "tile_radius") prefs.tile_radius.reset();
    if (key == "panel_radius") prefs.panel_radius.reset();
    if (key == "control_radius") prefs.control_radius.reset();
    if (key == "tile_spacing") prefs.tile_spacing.reset();
  }
  ApplyAppearance(prefs);
  api::SaveFrontendPrefsAsync(qApp, change, [](PatchConfigResult) {});
}

SetupWindow::SetupWindow(LibraryServices services, SetupWork* work, const FrontendPrefs& prefs,
                         QWidget* parent)
    : QWidget(parent, Qt::Window), prefs_(prefs) {
  setObjectName("setup_window");
  setWindowTitle("Set up Mira");
  setAttribute(Qt::WA_DeleteOnClose);
  context_.services = services;
  context_.work = work;
  context_.choices = &choices_;
  context_.art = new DemoArt(this);
  context_.prefs = &prefs_;
  if (const QScreen* screen = QGuiApplication::primaryScreen()) {
    context_.screen = setup::ScreenKind(screen->size(), screen->physicalSize());
  }

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // What's under way in the background, so Next never waits on it.
  auto* top = new QHBoxLayout();
  top->setContentsMargins(36, 10, 16, 0);
  top->addStretch(1);
  running_ = new QLabel(this);
  running_->setObjectName("setup_running");
  QSizePolicy keep = running_->sizePolicy();
  keep.setRetainSizeWhenHidden(true);
  running_->setSizePolicy(keep);
  top->addWidget(running_);
  layout->addLayout(top);

  stack_ = new QStackedWidget(this);
  layout->addWidget(stack_, /*stretch=*/1);

  auto* foot_box = new QWidget(this);
  foot_box->setObjectName("setup_foot");
  auto* foot = new QHBoxLayout(foot_box);
  foot->setContentsMargins(16, 12, 16, 12);
  foot->setSpacing(8);
  back_ = new QPushButton("Back", foot_box);
  back_->setObjectName("text_button");
  connect(back_, &QPushButton::clicked, this, [this] {
    // Back from Skip setup goes back to where it was pressed, with nothing dropped.
    if (choices_.skipped_all) {
      choices_ = setup::Choices{};
      return Go(0);
    }
    Go(current_ - 1);
  });
  foot->addWidget(back_);
  dots_ = new StepDots(foot_box);
  foot->addWidget(dots_, /*stretch=*/1);
  skip_ = new QPushButton("Skip", foot_box);
  skip_->setObjectName("text_button");
  connect(skip_, &QPushButton::clicked, this, &SetupWindow::Skip);
  foot->addWidget(skip_);
  next_ = new QPushButton("Next", foot_box);
  next_->setDefault(true);
  next_->setMinimumWidth(110);
  connect(next_, &QPushButton::clicked, this, &SetupWindow::Next);
  foot->addWidget(next_);
  layout->addWidget(foot_box);

  // Every page is made now, so the window takes the tallest one's height and never changes size.
  setup::Choices everything{.games = true, .apps = true};
  everything.stores = setup::kStores;
  for (const QString& key : setup::Flow(everything)) PageFor(key);

  connect(work, &SetupWork::Changed, this, &SetupWindow::ShowRunning);
  Go(0);
}

SetupStep* SetupWindow::PageFor(const QString& key) {
  if (SetupStep* page = pages_.value(key)) return page;
  SetupStep* page = nullptr;
  if (key == "welcome") page = new WelcomePage(context_);
  else if (key == "use") page = new UsePage(context_);
  else if (key == "found") page = new FoundPage(context_);
  else if (key == "stores") page = new StoresPage(context_);
  else if (key == "apps") page = new AppsPage(context_);
  else if (key.startsWith("sign:")) page = new StorePage(context_, key.mid(5));
  else if (key == "office") page = new OfficeSetupPage(context_);
  else if (key == "look") page = new LookPage(context_);
  else if (key == "sidebar") page = new SidebarPage(context_);
  else page = new DonePage(context_);
  connect(page, &SetupStep::Finished, this, [this, page] {
    if (stack_->currentWidget() != nullptr &&
        static_cast<QScrollArea*>(stack_->currentWidget())->widget() == page)
      Next();
  });
  connect(page, &SetupStep::PicksChanged, this, [this] {
    flow_ = setup::Flow(choices_);
    dots_->Set(static_cast<int>(flow_.size()), current_);
  });
  auto* scroll = new QScrollArea(stack_);
  scroll->setObjectName("setup_scroll");
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SetUpScrolling(scroll, page);
  scroll->setWidget(page);
  stack_->addWidget(scroll);
  pages_.insert(key, page);
  return page;
}

void SetupWindow::Go(int index) {
  flow_ = setup::Flow(choices_);
  if (index < 0 || index >= flow_.size()) return;
  current_ = index;
  const QString key = flow_[current_];
  SetupStep* page = PageFor(key);
  page->Enter();
  flow_ = setup::Flow(choices_);  // entering a page that was skipped brings its picks back
  stack_->setCurrentWidget(page->parentWidget()->parentWidget());
  static_cast<QScrollArea*>(stack_->currentWidget())->verticalScrollBar()->setValue(0);
  back_->setVisible(current_ > 0);
  skip_->setVisible(key != "done");
  skip_->setText(key == "welcome"                           ? "Skip setup"
                 : key.startsWith("sign:") || key == "office" ? "Set up later"
                                                               : "Skip");
  next_->setText(key == "done" ? "Open Mira" : key == "welcome" ? "Get started" : "Next");
  dots_->Set(static_cast<int>(flow_.size()), current_);
  next_->setFocus();
  ShowRunning();
}

void SetupWindow::Next() {
  const QString key = flow_.value(current_);
  if (key == "done") return Finish();
  pages_.value(key)->Leave();
  Go(current_ + 1);
}

void SetupWindow::Skip() {
  const QString key = flow_.value(current_);
  if (key == "welcome") {
    // Everything is left for later; the last page says where each thing is.
    choices_.games = false;
    choices_.apps = false;
    choices_.skipped_all = true;
    return Go(static_cast<int>(setup::Flow(choices_).size()) - 1);
  }
  if (key == "use") {
    choices_.games = false;
    choices_.apps = false;
  } else if (key == "found") {
    choices_.found.clear();
  } else if (key == "stores") {
    choices_.stores_skipped = true;
  } else if (key == "apps") {
    choices_.apps_skipped = true;
  }
  Go(current_ + 1);
}

void SetupWindow::Finish() {
  if (finished_) return;
  finished_ = true;
  bool big_screen = false;
  if (auto* done = static_cast<DonePage*>(pages_.value("done"))) {
    if (flow_.value(current_) == "done") done->Leave();
    big_screen = done->BigScreen();
  }
  if (flow_.value(current_) == "done" && !choices_.skipped_all) {
    // A source left unpicked goes off, unless games already came in from it.
    const QStringList on = setup::SourcesOn(choices_);
    QSet<QString> has_games;
    for (const GameSummary& game : context_.services.library->Games()) {
      has_games.insert(QString::fromStdString(SourceIdOf(game.source)));
    }
    QStringList off;
    for (const SourceInfo& source : AllSources()) {
      if (!on.contains(source.id) && !has_games.contains(source.id)) off << source.id;
    }
    context_.work->SetSourcesOn(on, off);
  }
  emit Finished(big_screen);
  close();
}

void SetupWindow::closeEvent(QCloseEvent* event) {
  if (!finished_) {
    finished_ = true;
    emit Finished(false);
  }
  QWidget::closeEvent(event);
}

void SetupWindow::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (!fitted_) FitHeight();
}

void SetupWindow::keyPressEvent(QKeyEvent* event) {
  // Enter wherever it isn't taken (a paste field signs in) moves on; Escape doesn't close by accident.
  if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
      qobject_cast<QLineEdit*>(focusWidget()) == nullptr)
    return Next();
  if (event->key() == Qt::Key_Escape) return event->accept();
  QWidget::keyPressEvent(event);
}

void SetupWindow::FitHeight() {
  fitted_ = true;
  constexpr int kWidth = 640;
  int tallest = 0;
  for (SetupStep* page : pages_) {
    page->ensurePolished();
    const int height = page->hasHeightForWidth() ? page->heightForWidth(kWidth)
                                                 : page->sizeHint().height();
    tallest = std::max(tallest, height);
  }
  int height = tallest + layout()->itemAt(0)->sizeHint().height() +
               layout()->itemAt(2)->widget()->sizeHint().height();
  if (const QScreen* screen = this->screen()) {
    height = std::min(height, screen->availableGeometry().height() - 60);
  }
  setFixedSize(kWidth, height);
}

void SetupWindow::ShowRunning() {
  const QStringList running = context_.work->Running();
  running_->setVisible(!running.isEmpty() && flow_.value(current_) != "done");
  running_->setText(QString("<span style='color:%1'>●</span>&nbsp; %2")
                        .arg(theme::Current().accent.name(), running.join("  ·  ").toHtmlEscaped()));
}

}  // namespace mira_gui
