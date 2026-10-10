#include "OfficePanel.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

#include "../client/EventHub.h"
#include "../client/Events.h"
#include "../client/api/Config.h"
#include "../client/api/Stores.h"
#include "../sources/SourceText.h"
#include "../sources/Sources.h"
#include "../system/PackageInstall.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ProgressRail.h"
#include "SetupLog.h"
#include "SignInGuide.h"

namespace mira_gui {
namespace {

// What most people open; the rest are a tick away.
bool PickedByDefault(const std::string& ref) {
  return ref == "word" || ref == "excel" || ref == "powerpoint";
}

}  // namespace

const std::vector<OfficePlan>& OfficePlans() {
  static const std::vector<OfficePlan> kPlans = {
      {"O365HomePremRetail", "Microsoft 365 Personal or Family", "Personal or Family"},
      {"O365BusinessRetail", "Business plans", "Business"},
      {"O365ProPlusRetail", "Apps for enterprise", "Enterprise"},
  };
  return kPlans;
}

OfficePanel::OfficePanel(QWidget* parent, bool header) : QWidget(parent) {
  const SignInGuide guide = GuideFor("office");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  if (header) {
    auto* head = new QHBoxLayout();
    head->setSpacing(10);
    auto* back = new QPushButton("Back", this);
    back->setObjectName("text_button");
    connect(back, &QPushButton::clicked, this, &OfficePanel::BackClicked);
    head->addWidget(back);
    head->addWidget(MakeSourceBadge(*FindSourceInfo("office"), 26, this));
    auto* title = new QLabel(guide.title, this);
    title->setProperty("role", "heading");
    head->addWidget(title);
    head->addStretch(1);
    done_label_ = new QLabel(QString::fromUtf8("\xe2\x9c\x93 ") + guide.done, this);
    done_label_->setStyleSheet(QString("color: %1;").arg(theme::Current().success.name()));
    done_label_->setVisible(false);
    head->addWidget(done_label_);
    layout->addLayout(head);
  }

  plan_box_ = new QWidget(this);
  auto* plan = new QVBoxLayout(plan_box_);
  plan->setContentsMargins(0, 0, 0, 0);
  plan->setSpacing(8);
  plan->addWidget(MakeLabel(plan_box_, "Which plan do you have?", "section"));
  auto* choices = new QHBoxLayout();
  choices->setSpacing(8);
  plans_ = new QButtonGroup(this);
  for (const OfficePlan& office_plan : OfficePlans()) {
    auto* choice = new QPushButton(office_plan.name, plan_box_);
    choice->setObjectName("choice_card");
    choice->setCheckable(true);
    choice->setProperty("plan", office_plan.value);
    plans_->addButton(choice);
    choices->addWidget(choice);
  }
  choices->addStretch(1);
  plan->addLayout(choices);
  connect(plans_, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* button) {
    api::PatchConfigAsync(
        this,
        {{"launchers.office.plan", "a string", button->property("plan").toString().toStdString()}},
        [this](PatchConfigResult result) {
          if (!result.ok) ShowError(error_, "Could not save the plan.", result.error);
        });
  });
  install_ = new QPushButton("Install Office", plan_box_);
  install_->setDefault(true);
  connect(install_, &QPushButton::clicked, this, &OfficePanel::Install);
  auto* install_row = new QHBoxLayout();
  install_row->addWidget(install_);
  install_row->addStretch(1);
  plan->addLayout(install_row);
  layout->addWidget(plan_box_);

  progress_box_ = new QWidget(this);
  auto* progress = new QVBoxLayout(progress_box_);
  progress->setContentsMargins(0, 0, 0, 0);
  progress->setSpacing(6);
  progress_text_ = MakeLabel(progress_box_, QString());
  progress->addWidget(progress_text_);
  rail_ = new ProgressRail(progress_box_);
  rail_->setMaximumWidth(460);
  progress->addWidget(rail_);
  progress_box_->setVisible(false);
  layout->addWidget(progress_box_);

  apps_box_ = new QWidget(this);
  auto* apps = new QVBoxLayout(apps_box_);
  apps->setContentsMargins(0, 0, 0, 0);
  apps->setSpacing(8);
  apps->addWidget(MakeLabel(apps_box_, "Add to your library", "section"));
  app_tiles_ = new QWidget(apps_box_);
  apps->addWidget(app_tiles_);
  add_ = new QPushButton(apps_box_);
  add_->setDefault(true);
  connect(add_, &QPushButton::clicked, this, &OfficePanel::AddApps);
  auto* add_row = new QHBoxLayout();
  add_row->addWidget(add_);
  add_row->addStretch(1);
  apps->addLayout(add_row);
  apps_box_->setVisible(false);
  layout->addWidget(apps_box_);

  auto* note = new QHBoxLayout();
  note->setSpacing(8);
  auto* icon = new QLabel(this);
  icon->setPixmap(icons::For(icons::Glyph::EyeSlash, theme::Current().text_muted)
                      .pixmap(QSize(14, 14), devicePixelRatioF()));
  note->addWidget(icon, 0, Qt::AlignTop);
  note->addWidget(MakeLabel(this, guide.note, "muted"), /*stretch=*/1);
  layout->addLayout(note);

  error_ = MakeLabel(this, QString(), "error");
  error_->setVisible(false);
  layout->addWidget(error_);
  log_ = new SetupLog("office", FindSourceInfo("office")->name, this);
  layout->addWidget(log_);

  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
  api::GetConfigAsync(this, [this](ConfigResult result) {
    const auto chosen = result.values.find("launchers.office.plan");
    for (QAbstractButton* button : plans_->buttons()) {
      button->setChecked(chosen != result.values.end() &&
                         button->property("plan").toString().toStdString() == chosen->second);
    }
  });
  Refresh();
}

void OfficePanel::Refresh() {
  api::GetLaunchersAsync(this, [this](LaunchersResult result) {
    if (!result.ok) return ShowError(error_, "Could not ask Mira about it.", result.error);
    for (const LauncherInfo& launcher : result.launchers) {
      if (launcher.id != "office") continue;
      const bool newly = launcher.installed && !installed_;
      installed_ = launcher.installed;
      installing_ = launcher.install_state == "running";
      if (newly) emit Changed();
    }
    ShowState();
    if (!installed_) return;
    api::GetStoreLibraryAsync(this, "office", false, [this](StoreLibraryResult listed) {
      if (!listed.ok) return ShowError(error_, "Could not list the apps.", listed.error);
      apps_ = listed.titles;
      if (picked_.isEmpty()) {
        for (const StoreTitle& app : apps_) {
          if (app.installed || PickedByDefault(app.ref))
            picked_.insert(QString::fromStdString(app.ref));
        }
      }
      ShowState();
    });
  });
}

void OfficePanel::ShowFailure(const QString& what, const ApiError& error) {
  ShowError(error_, what, error);
}

void OfficePanel::ShowState() {
  if (done_label_ != nullptr) done_label_->setVisible(installed_);
  plan_box_->setVisible(!installed_ && !installing_);
  progress_box_->setVisible(installing_ || !queue_.empty());
  if (installing_) {
    progress_text_->setText(progress_ < 0.4 ? "Getting the Windows parts ready…"
                                            : "Installing Office…");
    rail_->SetProgress(progress_);
  }
  apps_box_->setVisible(installed_ && !apps_.empty());
  if (!installed_) return;

  delete app_tiles_->layout();
  for (QWidget* child : app_tiles_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
    child->hide();
    child->deleteLater();
  }
  auto* grid = new QGridLayout(app_tiles_);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setSpacing(8);
  int to_add = 0;
  for (int i = 0; i < static_cast<int>(apps_.size()); ++i) {
    const StoreTitle& app = apps_[i];
    const QString ref = QString::fromStdString(app.ref);
    auto* tile = new QPushButton(QString::fromStdString(app.title), app_tiles_);
    tile->setObjectName("choice_card");
    tile->setCheckable(true);
    tile->setChecked(app.installed || picked_.contains(ref));
    tile->setEnabled(!app.installed);  // removing one is the app's own menu's job
    connect(tile, &QPushButton::toggled, this, [this, ref](bool on) {
      if (on) {
        picked_.insert(ref);
      } else {
        picked_.remove(ref);
      }
      ShowState();
    });
    grid->addWidget(tile, i / 4, i % 4);
    if (!app.installed && picked_.contains(ref)) ++to_add;
  }
  add_->setText(to_add == 1 ? "Add 1 app" : QString("Add %1 apps").arg(to_add));
  add_->setVisible(to_add > 0 || !queue_.empty());
  add_->setEnabled(queue_.empty());
  if (!queue_.empty())
    progress_text_->setText(QString("Adding apps, %1 to go…").arg(queue_.size()));
}

void OfficePanel::Install() {
  error_->setVisible(false);
  system::EnsurePackages(this, "winetricks", "Microsoft 365", [this](bool ready) {
    if (!ready) return;
    installing_ = true;
    progress_ = -1;
    ShowState();
    emit InstallStarted();
    api::InstallLauncherAsync(this, "office", [this](StoreActionResult started) {
      if (started.ok) return;
      installing_ = false;
      ShowState();
      ShowError(error_, "Could not start installing.", started.error);
      emit InstallFailed();
    });
  });
}

void OfficePanel::AddApps() {
  queue_.clear();
  for (const StoreTitle& app : apps_) {
    if (!app.installed && picked_.contains(QString::fromStdString(app.ref)))
      queue_.push_back(app.ref);
  }
  InstallNext();
}

void OfficePanel::InstallNext() {
  // Office's installer runs for the whole set each time, so one app at a time.
  if (queue_.empty()) return Refresh();
  rail_->SetProgress(-1);
  ShowState();
  api::InstallStoreTitleAsync(this, "office", queue_.front(), false,
                              [this](StoreActionResult started) {
                                if (started.ok) return;
                                queue_.clear();
                                ShowState();
                                ShowError(error_, "Could not add the app.", started.error);
                              });
}

void OfficePanel::HandleEvent(const std::string& type, const std::string& data) {
  StoreEvent event;
  if (!events::ParseStoreEvent(type, data, &event) || event.source != "office") return;
  if (event.kind == "setup") {
    if (event.state == "progress") {
      installing_ = true;
      progress_ = event.progress;
      ShowState();
    } else if (event.state == "finished" || event.state == "failed") {
      installing_ = false;
      if (event.state == "failed" && event.error.code != "cancelled") {
        ShowError(error_, "Installing failed.", event.error);
        log_->ShowLast();
      }
      Refresh();
    }
  } else if (event.kind == "install" && !queue_.empty() && event.ref == queue_.front()) {
    if (event.state == "progress") {
      rail_->SetProgress(event.progress);
    } else if (event.state == "finished") {
      queue_.erase(queue_.begin());
      InstallNext();
    } else if (event.state == "failed") {
      queue_.clear();
      ShowState();
      if (event.error.code != "cancelled") ShowError(error_, "Adding the app failed.", event.error);
    }
  }
}

}  // namespace mira_gui
