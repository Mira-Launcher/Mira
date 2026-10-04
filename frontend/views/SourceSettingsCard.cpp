#include "SourceSettingsCard.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "../client/MiradClient.h"
#include "../ui/ErrorHelp.h"
#include "../ui/Theme.h"

namespace mira_gui {
namespace {

bool IsSourceKey(const SourceInfo& source, const std::string& key) {
  const std::string id = source.id.toStdString();
  if (source.kind == SourceInfo::Kind::Launcher && key.starts_with("launchers." + id + ".")) return true;
  // enabled belongs to Manage sources, runner has its own row, collections their own dialog.
  return key.starts_with(id + ".") && key != id + ".enabled" && key != id + ".runner" &&
         key != "itch.collections";
}

QString Games(int count) { return QString("%1 game%2").arg(count).arg(count == 1 ? "" : "s"); }

}  // namespace

SourceSettingsCard::SourceSettingsCard(const SourceInfo& source, QWidget* parent)
    : SettingsCard(source.name + " settings", parent), source_(source), id_(source.id.toStdString()) {
  status_ = new QLabel(this);
  status_->setVisible(false);
  Header()->addWidget(status_);

  if (HasRunner()) BuildRunnerRow();

  footer_ = new QWidget(this);
  auto* footer = new QHBoxLayout(footer_);
  footer->setContentsMargins(12, 8, 18, 6);
  auto* all = new QPushButton("All settings…", footer_);
  all->setObjectName("text_button");
  all->setCursor(Qt::PointingHandCursor);
  connect(all, &QPushButton::clicked, this, [this] {
    emit OpenSettingsRequested(settings_.empty() ? QString() : QString::fromStdString(settings_.front().entry.key));
  });
  footer->addWidget(all);
  footer->addStretch(1);
  // Same pair, and names, as the settings screen's change bar.
  discard_ = new QPushButton("Discard", footer_);
  connect(discard_, &QPushButton::clicked, this, &SourceSettingsCard::Discard);
  footer->addWidget(discard_);
  save_ = new QPushButton("Save", footer_);
  save_->setDefault(true);
  connect(save_, &QPushButton::clicked, this, &SourceSettingsCard::Save);
  footer->addWidget(save_);
  AddRow(footer_);

  UpdateButtons();
  LoadSettings();
}

bool SourceSettingsCard::RunnerDirty() const {
  return runner_ != nullptr && runner_->isEnabled() && runner_->currentData().toString() != runner_ref_;
}

bool SourceSettingsCard::IsDirty() const {
  if (RunnerDirty()) return true;
  return std::ranges::any_of(settings_, [](const SettingEditor& editor) { return editor.Changed(); });
}

void SourceSettingsCard::UpdateButtons() {
  const bool dirty = IsDirty();
  discard_->setEnabled(dirty && !saving_);
  save_->setEnabled(dirty && !saving_);
  if (runner_row_ != nullptr) runner_row_->SetModified(RunnerDirty());
  for (const SettingEditor& editor : settings_) editor.row->SetModified(editor.Changed());
  // Switching every game to a runner that isn't saved yet would be a surprise.
  if (runner_apply_ != nullptr) runner_apply_->setVisible(runner_can_apply_ && !RunnerDirty());
}

void SourceSettingsCard::Discard() {
  if (runner_ != nullptr) SelectRunner(runner_ref_);
  for (SettingEditor& editor : settings_) editor.SetText(editor.original);
  status_->setVisible(false);
  UpdateButtons();
}

void SourceSettingsCard::Save() {
  if (saving_) return;  // the running save reports its own result
  if (!IsDirty()) {
    emit SaveFinished(true);
    return;
  }
  // As on the settings screen: a value put back to its default is removed, not written out.
  std::vector<ConfigEdit> edits;
  std::vector<size_t> edited;
  std::vector<size_t> resets;
  for (size_t i = 0; i < settings_.size(); ++i) {
    if (!settings_[i].Changed()) continue;
    if (settings_[i].IsDefault()) {
      resets.push_back(i);
    } else {
      edits.push_back({settings_[i].entry.key, settings_[i].entry.type, settings_[i].Text()});
      edited.push_back(i);
    }
  }
  saving_ = true;
  save_error_.clear();
  UpdateButtons();

  // Settings first, then the runner; a failed setting stops there.
  saves_pending_ = (edits.empty() ? 0 : 1) + static_cast<int>(resets.size());
  if (saves_pending_ == 0) {
    SaveRunner();
    return;
  }
  const auto done = [this](bool ok, const std::string& error, const std::vector<size_t>& indices) {
    if (ok) {
      for (const size_t i : indices) settings_[i].original = settings_[i].Text();
    } else if (save_error_.isEmpty()) {
      save_error_ = error_help::Describe(error);
    }
    if (--saves_pending_ > 0) return;
    if (!save_error_.isEmpty()) {
      ShowStatus(save_error_, true);
      FinishSave(false);
      return;
    }
    SaveRunner();
  };
  if (!edits.empty()) {
    MiradClient::PatchConfigAsync(this, edits, [done, edited](PatchConfigResult result) {
      done(result.ok, result.error, edited);
    });
  }
  for (const size_t i : resets) {
    MiradClient::ResetConfigKeyAsync(this, settings_[i].entry.key, [done, i](PatchConfigResult result) {
      done(result.ok, result.error, {i});
    });
  }
}

void SourceSettingsCard::SaveRunner() {
  if (!RunnerDirty()) {
    ShowStatus("Saved", false);
    FinishSave(true);
    return;
  }
  MiradClient::SetSourceRunnerAsync(this, id_, runner_->currentData().toString().toStdString(),
                                    /*apply_to_games=*/false, [this](SourceRunnerResult result) {
                                      if (!result.ok) {
                                        ShowStatus("Could not change the runner: " + error_help::Describe(result.error),
                                                   true);
                                        FinishSave(false);
                                        return;
                                      }
                                      ShowRunner(result);
                                      ShowStatus("Saved", false);
                                      FinishSave(true);
                                    });
}

void SourceSettingsCard::FinishSave(bool ok) {
  saving_ = false;
  UpdateButtons();
  emit SaveFinished(ok);
}

bool SourceSettingsCard::HasRunner() const {
  return source_.kind == SourceInfo::Kind::Launcher ||
         (source_.kind == SourceInfo::Kind::Store && id_ != "humble");
}

void SourceSettingsCard::Refresh() {
  LoadRunner();
  LoadSettings();
}

void SourceSettingsCard::BuildRunnerRow() {
  const QString runner_doc =
      source_.kind == SourceInfo::Kind::Launcher
          ? "The Wine or Proton build that " + source_.name + " and its games run with."
          : "The Wine or Proton build for " + source_.name + " games that have no runner of their own.";
  runner_row_ = new SettingRow("Runner", runner_doc, this);
  runner_ = new QComboBox(runner_row_);
  runner_->setEnabled(false);
  runner_->setMinimumWidth(260);
  connect(runner_, QOverload<int>::of(&QComboBox::activated), this, &SourceSettingsCard::UpdateButtons);
  connect(runner_row_, &SettingRow::RevertClicked, this, [this] {
    SelectRunner(runner_ref_);
    UpdateButtons();
  });
  runner_row_->AddControl(runner_);

  auto* note = new QWidget(runner_row_);
  auto* note_row = new QHBoxLayout(note);
  note_row->setContentsMargins(0, 0, 0, 0);
  runner_note_ = new QLabel(note);
  runner_note_->setWordWrap(true);
  runner_note_->setProperty("role", "muted");
  note_row->addWidget(runner_note_, /*stretch=*/1);
  runner_apply_ = new QPushButton(note);
  runner_apply_->setVisible(false);
  connect(runner_apply_, &QPushButton::clicked, this, [this] {
    runner_apply_->setEnabled(false);
    MiradClient::SetSourceRunnerAsync(this, id_, runner_ref_.toStdString(), /*apply_to_games=*/true,
                                      [this](SourceRunnerResult result) {
                                        runner_apply_->setEnabled(true);
                                        if (!result.ok) {
                                          ShowStatus("Could not switch the games: " + error_help::Describe(result.error), true);
                                          return;
                                        }
                                        ShowStatus("Switched", false);
                                        ShowRunner(result);
                                      });
  });
  note_row->addWidget(runner_apply_, 0, Qt::AlignTop);
  runner_row_->SetBelow(note);
  AddRow(runner_row_);
  LoadRunner();
}

void SourceSettingsCard::LoadRunner() {
  if (runner_ == nullptr) return;
  MiradClient::GetSourceRunnerAsync(this, id_, [this](SourceRunnerResult runner) { ShowRunner(runner); });
}

void SourceSettingsCard::SelectRunner(const QString& runner_ref) {
  int index = runner_->findData(runner_ref);
  if (index < 0) {
    runner_->addItem(runner_ref + " (not installed)", runner_ref);
    index = runner_->count() - 1;
  }
  runner_->setCurrentIndex(index);
}

void SourceSettingsCard::ShowRunner(const SourceRunnerResult& runner) {
  runner_can_apply_ = false;
  UpdateButtons();
  if (!runner.ok) {
    runner_->setEnabled(false);
    runner_note_->setText(source_.kind == SourceInfo::Kind::Launcher
                              ? "Install " + source_.name + " to choose its runner."
                              : "Could not ask mirad: " + QString::fromStdString(runner.error));
    return;
  }
  runner_ref_ = QString::fromStdString(runner.runner_ref);
  MiradClient::ListRunnersAsync(this, [this](RunnersResult runners) {
    runner_->clear();
    runner_->addItem("Default (from Runners settings)", QString());
    runner_->addItem("Auto (best available)", QString("auto"));
    if (runners.ok) {
      for (const RunnerInfo& info : runners.runners) {
        if (info.kind != "wine" && info.kind != "proton") continue;
        const QString label = QString::fromStdString(info.label.empty() ? info.name : info.label);
        runner_->addItem(QString("%1 (%2)").arg(label, QString::fromStdString(info.kind)),
                         QString::fromStdString(info.reference));
      }
    }
    SelectRunner(runner_ref_);
    runner_->setEnabled(true);
    UpdateButtons();
  });

  if (source_.kind == SourceInfo::Kind::Launcher) {
    runner_note_->setText(runner.games == 0
                              ? source_.name + " and the games it installs share one prefix and runner."
                              : source_.name + " and its " + Games(runner.games) +
                                    " share one prefix, so they switch together.");
  } else if (runner.games == 0) {
    runner_note_->setText("For " + source_.name + " games you install.");
  } else if (runner.differing == 0) {
    runner_note_->setText("All " + Games(runner.games) + " from " + source_.name + " use it.");
  } else {
    runner_note_->setText(QString("For new games and ones without their own runner. %1 of %2 games %3 another one.")
                              .arg(runner.differing)
                              .arg(runner.games)
                              .arg(runner.differing == 1 ? "uses" : "use"));
    runner_apply_->setText(runner.differing == 1 ? "Switch it too" : "Switch them too");
    runner_can_apply_ = true;
    UpdateButtons();
  }
}

void SourceSettingsCard::LoadSettings() {
  MiradClient::GetConfigSchemaAsync(this, [this](ConfigSchemaResult schema) {
    if (!schema.ok) {
      ShowStatus("Could not load settings: " + QString::fromStdString(schema.error), true);
      return;
    }
    if (settings_.empty()) {
      for (ConfigSchemaEntry& entry : schema.entries) {
        if (!IsSourceKey(source_, entry.key)) continue;
        SettingEditor editor;
        editor.entry = std::move(entry);
        settings_.push_back(std::move(editor));
      }
      for (size_t i = 0; i < settings_.size(); ++i) {
        SettingEditor& editor = settings_[i];
        AddRow(editor.Build(this));
        editor.OnEdited(this, [this] { UpdateButtons(); });
        connect(editor.row, &SettingRow::RevertClicked, this, [this, i] {
          settings_[i].SetText(settings_[i].original);
          UpdateButtons();
        });
      }
      MoveRow(footer_, static_cast<int>(Rows().size()) - 1);  // the buttons stay last
      MiradClient::ListRunnersAsync(this, [this](RunnersResult runners) {
        for (SettingEditor& editor : settings_) {
          if (editor.combo != nullptr && editor.entry.is_runner_ref) FillRunnerCombo(editor.combo, runners);
        }
      });
    }
    MiradClient::GetConfigAsync(this, [this](ConfigResult config) {
      if (!config.ok) {
        ShowStatus("Could not load settings: " + QString::fromStdString(config.error), true);
        return;
      }
      for (SettingEditor& editor : settings_) {
        const auto it = config.values.find(editor.entry.key);
        editor.original = it != config.values.end() ? it->second : editor.entry.default_display;
        editor.SetText(editor.original);
        editor.original = editor.Text();
      }
      UpdateButtons();
    });
  });
}

void SourceSettingsCard::ShowStatus(const QString& text, bool error) {
  theme::SetStyleProperty(status_, "role", error ? "error" : "muted");
  status_->setText(text);
  status_->setVisible(true);
}

}  // namespace mira_gui
