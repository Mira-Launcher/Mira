#include "SignInPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

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
#include "BrowserSketch.h"
#include "SetupLog.h"

namespace mira_gui {
namespace {

// A numbered step: its marker, then its text and whatever acts on it.
QWidget* StepRow(QWidget* parent, int number, const QString& text, const char* state,
                 QWidget* action) {
  auto* row = new QWidget(parent);
  auto* line = new QHBoxLayout(row);
  line->setContentsMargins(0, 0, 0, 0);
  line->setSpacing(10);
  auto* marker = new QLabel(
      qstrcmp(state, "done") == 0 ? QString::fromUtf8("\xe2\x9c\x93") : QString::number(number),
      row);
  marker->setObjectName("step_marker");
  marker->setProperty("state", state);
  marker->setFixedSize(22, 22);
  marker->setAlignment(Qt::AlignCenter);
  line->addWidget(marker, 0, Qt::AlignTop);
  auto* column = new QVBoxLayout();
  column->setSpacing(6);
  auto* label = MakeLabel(row, text);
  label->setObjectName("step_title");
  label->setProperty("state", state);
  label->setTextFormat(Qt::RichText);
  QFont font = label->font();
  font.setWeight(qstrcmp(state, "current") == 0 ? QFont::DemiBold : QFont::Normal);
  label->setFont(font);
  column->addWidget(label);
  if (action != nullptr) {
    auto* actions = new QHBoxLayout();
    actions->addWidget(action);
    actions->addStretch(1);
    column->addLayout(actions);
  }
  line->addLayout(column, /*stretch=*/1);
  return row;
}

QPushButton* Accent(const QString& text, QWidget* parent) {
  auto* button = new QPushButton(text, parent);
  button->setDefault(true);
  return button;
}

}  // namespace

SignInPanel::SignInPanel(const QString& id, QWidget* parent, bool header, bool stacked)
    : QWidget(parent), id_(id) {
  guide_ = GuideFor(id.toStdString());
  const SourceInfo* source = FindSourceInfo(id);
  mode_ = id == "steam"                                                     ? Mode::SteamKey
          : source != nullptr && source->kind == SourceInfo::Kind::Launcher ? Mode::Launcher
                                                                            : Mode::Store;

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  if (header) {
    auto* head = new QHBoxLayout();
    head->setSpacing(10);
    auto* back = new QPushButton("Back", this);
    back->setObjectName("text_button");
    connect(back, &QPushButton::clicked, this, &SignInPanel::BackClicked);
    head->addWidget(back);
    if (source != nullptr) head->addWidget(MakeSourceBadge(*source, 26, this));
    auto* title = new QLabel(guide_.title, this);
    title->setProperty("role", "heading");
    head->addWidget(title);
    head->addStretch(1);
    done_label_ = new QLabel(this);
    done_label_->setStyleSheet(QString("color: %1;").arg(theme::Current().success.name()));
    done_label_->setVisible(false);
    head->addWidget(done_label_);
    layout->addLayout(head);
  }

  if (!guide_.intro.isEmpty()) layout->addWidget(MakeLabel(this, guide_.intro, "muted"));

  // While the store's helper tool downloads; the steps wait for it.
  preparing_ = new QWidget(this);
  auto* preparing = new QVBoxLayout(preparing_);
  preparing->setContentsMargins(0, 4, 0, 4);
  preparing->addWidget(MakeLabel(preparing_, "Getting ready…"));
  auto* rail = new ProgressRail(preparing_);
  rail->setMaximumWidth(360);
  preparing->addWidget(rail);
  preparing_->setVisible(false);
  layout->addWidget(preparing_);

  body_ = new QWidget(this);
  auto* columns =
      new QBoxLayout(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight, body_);
  columns->setContentsMargins(0, 0, 0, 0);
  columns->setSpacing(stacked ? 12 : 24);
  auto* left = new QVBoxLayout();
  left->setSpacing(12);
  if (!guide_.other_steps.isEmpty()) {
    browsers_ = new QWidget(body_);
    browsers_->setObjectName("segmented");
    auto* pick = new QHBoxLayout(browsers_);
    pick->setContentsMargins(3, 3, 3, 3);
    pick->setSpacing(3);
    for (const auto& [label, other] :
         {std::pair{"Firefox", false}, std::pair{"Chrome or Edge", true}}) {
      auto* button = new QPushButton(label, browsers_);
      button->setCheckable(true);
      button->setAutoExclusive(true);
      button->setChecked(!other);
      connect(button, &QPushButton::clicked, this, [this, other] { ShowBrowser(other); });
      pick->addWidget(button);
    }
    auto* row = new QHBoxLayout();
    row->addWidget(browsers_);
    row->addStretch(1);
    left->addLayout(row);
  }
  steps_ = new QVBoxLayout();
  steps_->setSpacing(stacked ? 8 : 12);
  left->addLayout(steps_);

  account_row_ = new QWidget(body_);
  auto* account = new QHBoxLayout(account_row_);
  account->setContentsMargins(0, 0, 0, 0);
  account_ = MakeLabel(account_row_, QString(), "muted");
  account_->setTextFormat(Qt::RichText);
  account->addWidget(account_, /*stretch=*/1);
  account_choice_ = new QComboBox(account_row_);
  account_choice_->setVisible(false);
  account->addWidget(account_choice_);
  account_row_->setVisible(false);
  left->addWidget(account_row_);

  if (!guide_.note.isEmpty()) {
    auto* note = new QHBoxLayout();
    note->setSpacing(8);
    auto* icon = new QLabel(body_);
    icon->setPixmap(icons::For(guide_.note_private ? icons::Glyph::EyeSlash : icons::Glyph::Info,
                               theme::Current().text_muted)
                        .pixmap(QSize(14, 14), devicePixelRatioF()));
    note->addWidget(icon, 0, Qt::AlignTop);
    note->addWidget(MakeLabel(body_, guide_.note, "muted"), /*stretch=*/1);
    left->addLayout(note);
  }
  left->addStretch(1);
  columns->addLayout(left, /*stretch=*/1);

  auto* right = new QVBoxLayout();
  right->setSpacing(6);
  sketch_ = new BrowserSketch(body_);
  right->addWidget(sketch_);
  caption_ = MakeLabel(body_, QString(), "muted");
  caption_->setTextFormat(Qt::RichText);
  right->addWidget(caption_);
  right->addStretch(1);
  columns->addLayout(right, /*stretch=*/1);
  layout->addWidget(body_);

  // Picked up from the clipboard once the page is open; pasting is the fallback.
  foot_ = new QWidget(this);
  auto* foot = new QHBoxLayout(foot_);
  foot->setContentsMargins(0, 0, 0, 0);
  foot->setSpacing(10);
  watching_ =
      new QLabel(QString("<span style='color:%1'>●</span>&nbsp; Waiting for you to copy it…")
                     .arg(theme::Current().accent.name()),
                 foot_);
  watching_->setVisible(false);
  QSizePolicy keep = watching_->sizePolicy();
  keep.setRetainSizeWhenHidden(true);
  watching_->setSizePolicy(keep);
  foot->addWidget(watching_);
  foot->addStretch(1);
  paste_ = new QLineEdit(foot_);
  paste_->setPlaceholderText(mode_ == Mode::SteamKey ? "Paste the key here" : "Or paste it here");
  paste_->setMinimumWidth(240);
  connect(paste_, &QLineEdit::returnPressed, this, [this] { sign_in_->click(); });
  foot->addWidget(paste_);
  sign_in_ = new QPushButton(mode_ == Mode::SteamKey ? "Save key" : "Sign in", foot_);
  connect(sign_in_, &QPushButton::clicked, this, [this] {
    const QString text = paste_->text().trimmed();
    if (text.isEmpty()) return;
    if (mode_ == Mode::SteamKey) {
      SaveSteamKey(text);
    } else {
      SignIn(text, false);
    }
  });
  foot->addWidget(sign_in_);
  foot_->setVisible(mode_ != Mode::Launcher);
  layout->addWidget(foot_);

  error_ = MakeLabel(this, QString(), "error");
  error_->setVisible(false);
  layout->addWidget(error_);

  log_ = new SetupLog(id.toStdString(), source != nullptr ? source->name : id, this);
  connect(log_, &SetupLog::Progress, this, [this](double progress) {
    // mirad's own progress events win; the log's percentages fill in for installers without.
    if (install_progress_ < 0 && install_rail_ != nullptr) install_rail_->SetProgress(progress);
  });
  layout->addWidget(log_);

  ShowBrowser(false);
  if (mode_ == Mode::Store) {
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            &SignInPanel::CheckClipboard);
  }
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
}

void SignInPanel::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (!asked_) Refresh();
}

bool SignInPanel::eventFilter(QObject* watched, QEvent* event) {
  // On Wayland the clipboard only reaches Mira once its window is active again.
  if (event->type() == QEvent::WindowActivate) CheckClipboard();
  return QWidget::eventFilter(watched, event);
}

void SignInPanel::ShowBrowser(bool other) {
  other_browser_ = other;
  const Sketch& sketch = other ? guide_.other_sketch : guide_.sketch;
  sketch_->SetSketch(sketch);
  ShowLine(caption_, sketch.caption, "muted");
  ShowSteps();
}

void SignInPanel::ShowSteps() {
  while (QLayoutItem* item = steps_->takeAt(0)) {
    if (item->widget() != nullptr) item->widget()->deleteLater();
    delete item;
  }
  const QStringList& steps = other_browser_ ? guide_.other_steps : guide_.steps;
  const auto state = [](int i, int current) {
    return i < current ? "done" : i == current ? "current" : "todo";
  };

  if (mode_ == Mode::Launcher) {
    const int current = done_ ? 1 : 0;
    for (int i = 0; i < steps.size(); ++i) {
      QWidget* action = nullptr;
      if (i == 0 && !done_) {
        if (installing_) {
          install_rail_ = new ProgressRail(body_);
          install_rail_->SetProgress(install_progress_);
          install_rail_->setMinimumWidth(220);
          action = install_rail_;
        } else {
          auto* install = Accent("Install " + FindSourceInfo(id_)->name, body_);
          connect(install, &QPushButton::clicked, this, &SignInPanel::Install);
          action = install;
        }
      } else if (i == 1 && done_) {
        auto* open = Accent("Open " + FindSourceInfo(id_)->name, body_);
        connect(open, &QPushButton::clicked, this, [this] {
          api::OpenLauncherAsync(this, id_.toStdString(), [](StoreActionResult) {});
        });
        action = open;
      }
      steps_->addWidget(StepRow(body_, i + 1, steps[i], state(i, current), action));
    }
    return;
  }

  // The page to open, then what to do on it.
  const int last = static_cast<int>(steps.size());
  const int current = done_ ? last + 1 : opened_ ? last : 0;
  QPushButton* open = nullptr;
  if (!done_) {
    open = new QPushButton(guide_.open, body_);
    open->setDefault(!opened_);
    connect(open, &QPushButton::clicked, this, &SignInPanel::OpenPage);
  }
  steps_->addWidget(
      StepRow(body_, 1, opened_ || done_ ? "Opened the page." : "Open the page in your browser.",
              state(0, current), open));
  for (int i = 0; i < steps.size(); ++i) {
    steps_->addWidget(StepRow(body_, i + 2, steps[i], state(i + 1, current), nullptr));
  }
}

void SignInPanel::ShowDone(bool done, const QString& account) {
  const bool newly = done && !done_;
  const bool just_now = answered_;
  answered_ = true;
  done_ = done;
  QString text = guide_.done;
  if (done && !account.isEmpty()) text += " as " + account;
  if (done_label_ != nullptr) {
    done_label_->setText(QString::fromUtf8("\xe2\x9c\x93 ") + text);
    done_label_->setVisible(done);
  }
  foot_->setVisible(!done && mode_ != Mode::Launcher);
  watching_->setVisible(!done && opened_ && mode_ == Mode::Store);
  if (done) error_->setVisible(false);
  ShowSteps();
  if (newly) emit Connected(just_now);
}

void SignInPanel::SetPreparing(bool preparing) {
  preparing_->setVisible(preparing);
  body_->setVisible(!preparing);
  foot_->setVisible(!preparing && !done_ && mode_ != Mode::Launcher);
}

void SignInPanel::Refresh() {
  asked_ = true;
  const std::string id = id_.toStdString();
  if (mode_ == Mode::Store) {
    api::GetStoreStatusAsync(this, id, [this, id](StoreStatusResult status) {
      if (!status.ok) return ShowFailure("Could not ask Mira about it.", status.error);
      if (status.tool_installed) {
        SetPreparing(false);
        ShowDone(status.authenticated, QString::fromStdString(status.account));
        return;
      }
      // The helper tool comes first, without asking: it's how Mira talks to the store at all.
      SetPreparing(true);
      api::SetupStoreToolAsync(this, id, [this](StoreActionResult result) {
        if (result.ok) return Refresh();
        SetPreparing(false);
        ShowFailure("Could not get ready.", result.error);
        log_->ShowLast();
      });
    });
  } else if (mode_ == Mode::Launcher) {
    api::GetLaunchersAsync(this, [this](LaunchersResult result) {
      if (!result.ok) return ShowFailure("Could not ask Mira about it.", result.error);
      for (const LauncherInfo& launcher : result.launchers) {
        if (QString::fromStdString(launcher.id) != id_) continue;
        installing_ = launcher.install_state == "running";
        ShowDone(launcher.installed);
        log_->Watch(installing_);
        if (launcher.install_state == "failed" && !launcher.error.empty()) {
          ShowFailure("The last install failed.", launcher.error);
          log_->ShowLast();
        }
      }
    });
  } else {
    api::GetConfigAsync(this, [this](ConfigResult result) {
      if (!result.ok) return ShowFailure("Could not ask Mira about it.", result.error);
      const auto key = result.values.find("steam.web_api_key");
      ShowDone(key != result.values.end() && !key->second.empty());
    });
    api::GetSteamAccountsAsync(this, [this](SteamAccountsResult result) {
      if (!result.ok || result.accounts.empty()) return;
      accounts_ = result.accounts;
      const auto name = [](const SteamAccount& account) {
        return QString::fromStdString(account.persona_name.empty() ? account.account_name
                                                                   : account.persona_name);
      };
      account_row_->setVisible(true);
      if (accounts_.size() == 1) {
        account_->setText(QString("Uses the Steam account <b>%1</b>, found on this computer.")
                              .arg(name(accounts_.front()).toHtmlEscaped()));
        return;
      }
      // Several people use Steam here: whose games to list is theirs to say.
      account_->setText("Steam account");
      account_choice_->clear();
      for (const SteamAccount& account : accounts_) {
        account_choice_->addItem(name(account), QString::fromStdString(account.steamid64));
        if (account.steamid64 == result.selected)
          account_choice_->setCurrentIndex(account_choice_->count() - 1);
      }
      account_choice_->setVisible(true);
    });
  }
}

void SignInPanel::OpenPage() {
  error_->setVisible(false);
  const auto opened = [this](const std::string& url) {
    QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
    opened_ = true;
    // Whatever was copied before the page opened isn't the answer.
    last_clipboard_ = QApplication::clipboard()->text();
    if (window() != nullptr) window()->installEventFilter(this);
    watching_->setVisible(mode_ == Mode::Store);
    ShowSteps();
  };
  if (mode_ == Mode::SteamKey) {
    api::GetConfigSchemaAsync(this, [this, opened](ConfigSchemaResult result) {
      if (!result.ok) return ShowFailure("Could not find the page.", result.error);
      for (const ConfigSchemaEntry& entry : result.entries) {
        if (entry.key == "steam.web_api_key" && !entry.link.empty()) return opened(entry.link);
      }
    });
    return;
  }
  api::BeginStoreLoginAsync(this, id_.toStdString(), [this, opened](LoginUrlResult result) {
    if (!result.ok) return ShowFailure("Could not start signing in.", result.error);
    opened(result.url);
  });
}

void SignInPanel::CheckClipboard() {
  if (!opened_ || done_ || mode_ != Mode::Store || !isVisible()) return;
  const QString text = QApplication::clipboard()->text();
  if (text == last_clipboard_ || text.trimmed().isEmpty() || text.size() > 20000) return;
  last_clipboard_ = text;
  api::FindStoreCredentialAsync(this, id_.toStdString(), text.toStdString(),
                                [this](FoundCredentialResult result) {
                                  if (result.ok && !result.credential.empty() && !done_)
                                    SignIn(QString::fromStdString(result.credential), true);
                                });
}

void SignInPanel::SignIn(const QString& text, bool from_clipboard) {
  sign_in_->setEnabled(false);
  sign_in_->setText("Signing in…");
  error_->setVisible(false);
  api::SignInStoreAsync(this, id_.toStdString(), text.toStdString(),
                        [this, from_clipboard](StoreActionResult result) {
                          sign_in_->setEnabled(true);
                          sign_in_->setText("Sign in");
                          if (!result.ok) {
                            return ShowFailure(from_clipboard
                                                   ? "Found what you copied, but signing in failed."
                                                   : "Could not sign in.",
                                               result.error);
                          }
                          paste_->clear();
                          // So the next store's page can't pick up the same credential.
                          if (from_clipboard) QApplication::clipboard()->clear();
                          Refresh();
                        });
}

void SignInPanel::SaveSteamKey(const QString& key) {
  std::vector<ConfigEdit> edits = {{"steam.web_api_key", "a string", key.toStdString()}};
  if (account_choice_->isVisible()) {
    edits.push_back(
        {"steam.steamid64", "a string", account_choice_->currentData().toString().toStdString()});
  }
  sign_in_->setEnabled(false);
  api::PatchConfigAsync(this, edits, [this](PatchConfigResult result) {
    sign_in_->setEnabled(true);
    if (!result.ok) return ShowFailure("Could not save the key.", result.error);
    paste_->clear();
    ShowDone(true);
  });
}

void SignInPanel::Install() {
  error_->setVisible(false);
  api::GetLaunchersAsync(this, [this](LaunchersResult result) {
    std::string packages;
    for (const LauncherInfo& launcher : result.launchers) {
      if (QString::fromStdString(launcher.id) == id_) packages = launcher.packages;
    }
    const auto start = [this] {
      installing_ = true;
      install_progress_ = -1;
      ShowSteps();
      log_->Watch(true);
      emit InstallStarted();
      api::InstallLauncherAsync(this, id_.toStdString(), [this](StoreActionResult started) {
        if (started.ok) return;  // its events tell the rest
        installing_ = false;
        ShowSteps();
        log_->Watch(false);
        ShowFailure("Could not start installing.", started.error);
        emit InstallFailed();
      });
    };
    if (packages.empty()) return start();
    system::EnsurePackages(this, packages, FindSourceInfo(id_)->name, [start](bool ready) {
      if (ready) start();
    });
  });
}

void SignInPanel::HandleEvent(const std::string& type, const std::string& data) {
  if (mode_ != Mode::Launcher) return;
  StoreEvent event;
  if (!events::ParseStoreEvent(type, data, &event) || event.kind != "setup" ||
      QString::fromStdString(event.source) != id_) {
    return;
  }
  if (event.state == "progress") {
    install_progress_ = event.progress;
    if (installing_ && install_rail_ != nullptr) return install_rail_->SetProgress(event.progress);
    installing_ = true;
    ShowSteps();
    log_->Watch(true);
  } else if (event.state == "finished") {
    installing_ = false;
    log_->Watch(false);
    Refresh();
  } else if (event.state == "failed") {
    installing_ = false;
    ShowSteps();
    log_->Watch(false);
    if (event.error.code != "cancelled") ShowFailure("Installing failed.", event.error);
  }
}

void SignInPanel::ShowFailure(const QString& what, const ApiError& error) {
  ShowError(error_, what, error);
}

}  // namespace mira_gui
