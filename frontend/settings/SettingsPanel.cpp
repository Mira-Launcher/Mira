#include "SettingsPanel.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStorageInfo>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <utility>

#include "../app/Display.h"
#include "../app/ErrorHelp.h"
#include "../app/KeyBindings.h"
#include "../app/Notify.h"
#include "../bigscreen/Session.h"
#include "../client/EventHub.h"
#include "../client/JsonMapping.h"
#include "../client/api/Config.h"
#include "../client/api/Runners.h"
#include "../client/api/Stores.h"
#include "../library/HiddenLock.h"
#include "../sources/Sources.h"
#include "../theme/Theme.h"
#include "../widgets/ShortcutEdit.h"
#include "AppearancePreviews.h"
#include "SettingsCard.h"
#include "SettingsNav.h"

namespace mira_gui {

SettingsPanel::SettingsPanel(Previews previews, QWidget* parent)
    : QWidget(parent), previews_(std::move(previews)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  nav_ = new SettingsNavWidget(this);
  layout->addWidget(nav_, /*stretch=*/1);

  change_bar_ = new ChangeBar(nav_->ContentArea());
  connect(change_bar_, &ChangeBar::DiscardClicked, this, &SettingsPanel::DiscardChanges);
  connect(change_bar_, &ChangeBar::SaveClicked, this, &SettingsPanel::Save);

  BuildInterfacePage();
  BuildBigScreenPage();
  BuildSidebarPage();
  BuildShortcutsPage();

  setEnabled(false);
  LoadFrontendPrefs();
  Load();
}

void SettingsPanel::SetHeader(QWidget* header) {
  nav_->SetHeaderWidget(header);
}

void SettingsPanel::SetNavWidth(int width) {
  nav_->SetNavWidth(width);
}

void SettingsPanel::LoadFrontendPrefs() {
  theme_saved_ = theme::CurrentName();
  SelectTheme(theme_saved_);

  api::GetFrontendPrefsAsync(this, [this](FrontendPrefsResult result) {
    if (!result.ok) {
      LoadDone();  // the defaults are already shown
      return;
    }
    const FrontendPrefs& prefs = result.prefs;
    if (prefs.theme) {
      theme_saved_ = QString::fromStdString(*prefs.theme);
      SelectTheme(theme_saved_);
    }
    for (PrefToggle& toggle : toggles_) {
      toggle.saved = (prefs.*toggle.member).value_or(toggle.fallback);
      toggle.toggle->setChecked(toggle.saved);
    }
    for (PrefSeconds& seconds : seconds_) {
      seconds.spin->setValue((prefs.*seconds.member).value_or(seconds.fallback) / 1000.0);
      seconds.saved = qRound(seconds.spin->value() * 1000);  // after the clamp
    }
    for (PrefChoice& choice : choices_) {
      choice.load(prefs);
      choice.saved = choice.combo->currentData();
    }
    continue_count_->setValue(prefs.library_continue_count.value_or(3));
    continue_count_saved_ = continue_count_->value();  // after the clamp
    trailer_volume_->setValue(prefs.big_screen_trailer_volume.value_or(50));
    trailer_volume_saved_ = trailer_volume_->value();
    for (ShapeField* field :
         {&tile_spacing_, &grid_margin_, &tile_radius_, &panel_radius_, &control_radius_}) {
      field->saved = field->current = prefs.*(field->member);
      RefreshShape(*field);
    }

    SidebarStyleChoices::Choices choices;
    choices.pinned = sidebar::ParseStyle(prefs.sidebar_pinned_style.value_or(""));
    choices.recent = sidebar::ParseStyle(prefs.sidebar_recent_style.value_or(""));
    choices.recent_count = std::clamp(prefs.sidebar_recent_count.value_or(0), 0, 10);
    choices.recent_when = prefs.sidebar_recent_when.value_or(true);
    sidebar_style_saved_ = choices;
    sidebar_style_->SetChoices(choices);

    Refresh();
    LoadDone();
  });
}

void SettingsPanel::LoadDone() {
  if (--loads_pending_ == 0) emit Ready();
}

// --- Schema pages ---------------------------------------------------------

void SettingsPanel::Load() {
  api::GetConfigSchemaAsync(this, [this](ConfigSchemaResult schema) {
    if (!schema.ok) {
      emit LoadFailed(error_help::Describe(schema.error));
      return;
    }
    for (ConfigSchemaEntry& entry : schema.entries) {
      // The Tags page shows its own settings (TagsPage), and Library its Mira folders.
      if (entry.game_only || entry.category == "Tags" || entry.key == "mira_folders") continue;
      SettingEditor field;
      field.entry = std::move(entry);
      fields_.push_back(std::move(field));
    }
    BuildSchemaPages();

    const auto list_runners = [this] {
      api::ListRunnersAsync(this, [this](RunnersResult result) { PopulateRunnerCombos(result); });
    };
    list_runners();
    connect(EventHub::Instance(), &EventHub::RunnersChanged, this, list_runners);

    api::GetConfigAsync(this, [this](ConfigResult config) {
      if (!config.ok) {
        emit LoadFailed(error_help::Describe(config.error));
        return;
      }
      ShowMiraFolders(config);
      for (SettingEditor& field : fields_) {
        const auto it = config.values.find(field.entry.key);
        field.original = it != config.values.end() ? it->second : field.entry.default_display;
        field.SetText(field.original);
        field.original = field.Text();  // as the editor holds it (a clamped number, a joined list)
      }
      nav_->RearrangePages();  // lists and paths now hold their rows, so cards have their real
                               // heights
      setEnabled(true);
      Refresh();
      LoadDone();
      if (!pending_focus_key_.isEmpty()) FocusKey(std::exchange(pending_focus_key_, QString()));
    });
  });
}

void SettingsPanel::BuildSchemaPages() {
  std::vector<std::string> categories;
  for (const SettingEditor& field : fields_) categories.push_back(field.entry.category);

  for (const auto& [category, rows] : GroupByCategory(categories)) {
    SettingsPage* page =
        nav_->AddCategory(category, CategoryGlyph(category), CategoryNavGroup(category));
    pages_[category] = page;
    const bool by_source =
        std::ranges::all_of(rows, [this](size_t i) { return !fields_[i].entry.source.empty(); });
    if (by_source) {
      // Which sources the sidebar shows, and in what order, sits with the sources themselves.
      if (category == "Sources" && sources_card_ != nullptr) page->AddWidget(sources_card_);
      BuildSourceCards(page, rows);
      continue;
    }
    if (category == "Library") AddMiraFoldersCard(page);
    SettingsCard* card = nullptr;
    int group = -1;
    std::vector<size_t> card_rows;
    const auto finish_card = [&] {
      if (card != nullptr && fields_[card_rows.front()].entry.group_resettable) {
        AddCardReset(card, {}, card_rows);
      }
      card_rows.clear();
    };
    for (const size_t i : rows) {
      if (card == nullptr || fields_[i].entry.group != group) {
        finish_card();
        group = fields_[i].entry.group;
        card = page->AddCard(QString::fromStdString(fields_[i].entry.group_label));
        if (fields_[i].entry.group_collapsed) card->SetCollapsible(/*collapsed=*/true);
      }
      AddSchemaRow(card, i);
      card_rows.push_back(i);
      // The password sits with the settings for when hidden games lock again, before them.
      if (fields_[i].entry.key.starts_with("hidden.") &&
          card->findChild<HiddenPasswordRow*>() == nullptr) {
        auto* password = new HiddenPasswordRow(card);
        card->AddRow(password);
        card->MoveRow(password, 0);
        nav_->RegisterRow(password, "hidden games password lock");
      }
    }
    finish_card();

    if (category == "Launching") {
      SettingsCard* first = page->findChild<SettingsCard*>();
      auto* row = new SettingRow("GameMode", {});
      gamemode_status_ = new QLabel("Checking…", row);
      theme::SetStyleProperty(gamemode_status_, "role", "subtle");
      row->AddControl(gamemode_status_);
      first->AddRow(row);
      nav_->RegisterRow(row, "gamemode feral daemon status");
      LoadGameModeStatus();
    }
  }
  rows_built_ = true;
  for (const SectionAction& action : section_actions_) AppendSectionAction(action);
}

void SettingsPanel::BuildSourceCards(SettingsPage* page, const std::vector<size_t>& rows) {
  std::vector<std::string> sources;
  for (const size_t i : rows) {
    if (std::ranges::find(sources, fields_[i].entry.source) == sources.end())
      sources.push_back(fields_[i].entry.source);
  }
  for (const std::string& source_id : sources) {
    const QString id = QString::fromStdString(source_id);
    const SourceInfo* source = FindSourceInfo(id);
    auto* card = new SettingsCard(source != nullptr ? source->name : id);
    source_cards_[id] = card;
    page->AddWidget(card);
    if (source != nullptr) card->SetLeading(MakeSourceBadge(*source, 24, card));
    for (const size_t i : rows) {
      if (fields_[i].entry.source != source_id) continue;
      AddSchemaRow(card, i);
    }
  }
  // Sources not set up yet are added from the sidebar, so their cards stay hidden.
  api::ListSourcesAsync(this, [this](SourcesResult result) {
    if (!result.ok) return;
    QSet<QString> added{"local"};
    for (const SourceState& source : result.sources) {
      if (source.added) added.insert(QString::fromStdString(source.id));
    }
    for (const auto& [id, card] : source_cards_) card->setVisible(added.contains(id));
  });
}

SettingRow* SettingsPanel::AddSchemaRow(SettingsCard* card, size_t index) {
  SettingEditor& field = fields_[index];
  SettingRow* row = field.Build(card);
  card->AddRow(row);
  nav_->RegisterRow(row, field.SearchText());
  field.OnEdited(this, [this] { Refresh(); });
  connect(row, &SettingRow::RevertClicked, this, [this, index] {
    fields_[index].SetText(fields_[index].original);
    Refresh();
  });
  return row;
}

void SettingsPanel::FocusKey(const QString& key) {
  if (key == kSidebarKey) {
    nav_->RevealRow(sidebar_style_->Rows().front());
    return;
  }
  if (key == kShortcutsKey) {
    if (SettingsPage* page = pages_.value("Shortcuts")) nav_->RevealPage(page);
    return;
  }
  if (key == kSidebarSourcesKey) {
    if (!rows_built_) {
      pending_focus_key_ = key;  // the card moves to the Sources page once the schema loads
    } else if (SettingsPage* page = pages_.value("Sources")) {
      nav_->RevealPage(page);  // the card is the page's first
    }
    return;
  }
  const std::string wanted = key.toStdString();
  const auto it =
      std::ranges::find(fields_, wanted, [](const SettingEditor& f) { return f.entry.key; });
  if (it == fields_.end() || it->row == nullptr) {
    pending_focus_key_ = key;  // the schema hasn't loaded yet, most likely
    return;
  }
  nav_->RevealRow(it->row);
  QWidget* input = it->Input();
  if (input == nullptr) return;
  input->setFocus(Qt::OtherFocusReason);
  if (auto* line = qobject_cast<QLineEdit*>(input)) line->selectAll();
}

void SettingsPanel::AddSectionAction(const QString& category, const QString& card,
                                     const QString& label, const QString& doc,
                                     const QString& button_text, std::function<void()> activated) {
  section_actions_.push_back({category, card, label, doc, button_text, std::move(activated)});
  if (rows_built_) AppendSectionAction(section_actions_.back());
}

void SettingsPanel::AppendSectionAction(const SectionAction& action) {
  SettingsPage* page = pages_.value(action.category);
  if (page == nullptr) return;
  const QList<SettingsCard*> cards = page->findChildren<SettingsCard*>();
  if (cards.isEmpty()) return;
  const auto titled = std::ranges::find(cards, action.card, &SettingsCard::Title);
  SettingsCard* card = titled != cards.end() ? *titled : cards.back();
  auto* row = new SettingRow(action.label, action.doc);
  auto* button = new QPushButton(action.button_text, row);
  connect(button, &QPushButton::clicked, this, action.activated);
  row->AddControl(button);
  card->AddRow(row);
  nav_->RegisterRow(row, QString("%1 %2 %3").arg(action.label, action.category, action.doc));
}

void SettingsPanel::LoadGameModeStatus() {
  api::GetGameModeStatusAsync(this, [this](GameModeStatusResult result) {
    if (gamemode_status_ == nullptr) return;
    QString text;
    bool error = false;
    if (!result.ok) {
      text = "Could not check (mirad unreachable).";
      error = true;
    } else if (!result.installed) {
      text = "Not installed: gamemoded isn't on PATH.";
      error = true;
    } else if (!result.daemon_running) {
      text = "Installed, but the daemon isn't running right now.";
      error = true;
    } else {
      text = "Installed and running.";
    }
    gamemode_status_->setText(text);
    theme::SetStyleProperty(gamemode_status_, "role", error ? "error" : "subtle");
  });
}

void SettingsPanel::PopulateRunnerCombos(const RunnersResult& result) {
  for (SettingEditor& field : fields_) {
    if (field.combo != nullptr && field.entry.is_runner_ref) FillRunnerCombo(field.combo, result);
  }
}

// --- Changes --------------------------------------------------------------

int SettingsPanel::ChangeCount() const {
  int count = 0;
  for (const PrefField& field : pref_fields_) count += field.changed() ? 1 : 0;
  for (const SettingEditor& field : fields_)
    count += field.row != nullptr && field.Changed() ? 1 : 0;
  return count;
}

bool SettingsPanel::PrefsDirty() const {
  return std::ranges::any_of(pref_fields_, [](const PrefField& field) { return field.changed(); });
}

void SettingsPanel::Refresh() {
  for (const PrefField& field : pref_fields_) {
    if (field.row == nullptr) continue;
    field.row->SetModified(field.changed());
  }
  for (const SettingEditor& field : fields_) {
    if (field.row != nullptr) field.row->SetModified(field.Changed());
  }
  for (const CardReset& reset : card_resets_) {
    const bool all_default =
        std::ranges::all_of(reset.prefs,
                            [this](size_t i) { return pref_fields_[i].is_default(); }) &&
        std::ranges::all_of(reset.schema, [this](size_t i) { return fields_[i].IsDefault(); });
    reset.card->SetResettable(!all_default);
  }
  change_bar_->SetCount(ChangeCount());
  UpdatePreviews();
}

void SettingsPanel::DiscardChanges() {
  for (const PrefField& field : pref_fields_) {
    if (field.changed()) field.revert();
  }
  for (SettingEditor& field : fields_) {
    if (field.Changed()) field.SetText(field.original);
  }
  Refresh();
}

void SettingsPanel::Save() {
  // Qt fires neither of two actions that share a key.
  for (std::size_t i = 0; i < shortcuts_.size(); ++i) {
    const QKeySequence keys = shortcuts_[i].edit->Keys();
    for (std::size_t j = i + 1; j < shortcuts_.size() && !keys.isEmpty(); ++j) {
      if (shortcuts_[j].edit->Keys() != keys) continue;
      notify::Failed(this, "Couldn't save shortcuts",
                     keys.toString(QKeySequence::NativeText) +
                         " is used by two shortcuts. Change one of them.");
      return;
    }
  }
  change_bar_->SetBusy(true);
  save_error_.clear();

  std::optional<FrontendPrefs> prefs_to_save;
  if (PrefsDirty()) {
    FrontendPrefs prefs;
    for (const PrefToggle& toggle : toggles_) prefs.*toggle.member = toggle.toggle->isChecked();
    for (const PrefSeconds& seconds : seconds_)
      prefs.*seconds.member = qRound(seconds.spin->value() * 1000);
    for (const PrefChoice& choice : choices_) choice.store(prefs);
    prefs.library_continue_count = continue_count_->value();
    prefs.big_screen_trailer_volume = trailer_volume_->value();
    const QString theme_name = SelectedTheme();
    prefs.theme = theme_name.toStdString();

    bool shapes_changed = false;
    theme::Overrides overrides;
    const std::pair<ShapeField*, const char*> shapes[] = {{&tile_spacing_, "tile_spacing"},
                                                          {&grid_margin_, "grid_margin"},
                                                          {&tile_radius_, "tile_radius"},
                                                          {&panel_radius_, "panel_radius"},
                                                          {&control_radius_, "control_radius"}};
    for (const auto& [field, key] : shapes) {
      shapes_changed = shapes_changed || field->current != field->saved;
      // Unset is "theme default": deleted from the file, not stored.
      if (field->current) {
        prefs.*(field->member) = *field->current;
      } else {
        prefs.clear.push_back(key);
      }
    }
    overrides.tile_spacing = tile_spacing_.current;
    overrides.grid_margin = grid_margin_.current;
    overrides.radius_tile = tile_radius_.current;
    overrides.radius_panel = panel_radius_.current;
    overrides.radius_control = control_radius_.current;
    if (shapes_changed || theme_name != theme_saved_)
      theme::Configure(theme_name, overrides);  // one restyle

    bool shortcuts_changed = false;
    for (const ShortcutField& field : shortcuts_) {
      if (field.edit->Keys() == field.saved) continue;
      shortcuts_changed = true;
      if (field.edit->Keys() == field.default_keys) {
        keybindings::ResetOverride(field.id);
      } else {
        keybindings::SetOverride(field.id, field.edit->Keys());
      }
    }
    if (shortcuts_changed) prefs.shortcut_overrides = keybindings::Current();

    const SidebarStyleChoices::Choices& style = sidebar_style_->Current();
    prefs.sidebar_pinned_style = sidebar::StyleKey(style.pinned);
    prefs.sidebar_recent_style = sidebar::StyleKey(style.recent);
    prefs.sidebar_recent_count = style.recent_count;
    prefs.sidebar_recent_when = style.recent_when;

    for (const PrefField& field : pref_fields_) field.mark_saved();
    prefs_to_save = prefs;
    emit PrefsSaved(prefs);
  }

  // A setting put back to its default is removed from settings.toml rather than
  // written out, so it follows the default if that changes later.
  std::vector<ConfigEdit> edits;
  std::vector<size_t> edited;
  std::vector<size_t> resets;
  for (size_t i = 0; i < fields_.size(); ++i) {
    const SettingEditor& field = fields_[i];
    if (field.row == nullptr || !field.Changed()) continue;
    if (field.IsDefault()) {
      resets.push_back(i);
    } else {
      edits.push_back({field.entry.key, field.entry.type, field.Text()});
      edited.push_back(i);
    }
  }
  // The display settings count too, so Save doesn't report done (and close) before they fail.
  saves_pending_ =
      (edits.empty() ? 0 : 1) + static_cast<int>(resets.size()) + (prefs_to_save ? 1 : 0);
  if (saves_pending_ == 0) {
    FinishSave(true, {});
    return;
  }
  // What was sent: an edit typed while the save runs is still unsaved after it.
  std::vector<std::string> sent(fields_.size());
  for (size_t i = 0; i < fields_.size(); ++i) sent[i] = fields_[i].Text();
  const auto done = [this, sent](bool ok, const std::string& error,
                                 const std::vector<size_t>& indices) {
    if (ok) {
      for (const size_t i : indices) fields_[i].original = sent[i];
    } else if (save_error_.isEmpty()) {
      save_error_ = error.empty() ? QString("Could not save.") : QString::fromStdString(error);
    }
    if (--saves_pending_ == 0) FinishSave(save_error_.isEmpty(), save_error_);
  };
  if (prefs_to_save) {
    const bool start_on_login = prefs_to_save->start_on_login.value_or(false);
    api::SaveFrontendPrefsAsync(
        this, *prefs_to_save, [this, done, start_on_login](PatchConfigResult result) {
          if (result.ok && !bigscreen::ApplyStartOnLogin(start_on_login)) {
            notify::Warn(this, "Could not change Mira's login entry in ~/.config/autostart.");
          }
          done(result.ok,
               result.error.message.empty() ? "" : "Display settings: " + result.error.message, {});
        });
  }
  if (!edits.empty()) {
    api::PatchConfigAsync(this, edits, [done, edited](PatchConfigResult result) {
      done(result.ok, result.error, edited);
    });
  }
  for (const size_t i : resets) {
    api::ResetConfigKeyAsync(this, fields_[i].entry.key, [done, i](PatchConfigResult result) {
      done(result.ok, result.error, {i});
    });
  }
}

void SettingsPanel::FinishSave(bool ok, const QString& error) {
  change_bar_->SetBusy(false);
  Refresh();
  emit SaveFinished(ok, error);
  // Once everything is saved: display settings take a restart.
  if (std::exchange(restart_prompt_, false) && ok &&
      notify::Confirm(this, "Restart Mira?", "Mira applies this the next time it opens.",
                      "Restart now")) {
    display::Restart({"--desktop"});
  }
}

// --- Mira folders ----------------------------------------------------------

namespace {

QStringList ListValue(const ConfigResult& config, const std::string& key) {
  QStringList list;
  const auto it = config.values.find(key);
  if (it == config.values.end()) return list;
  for (const std::string& item : mapping::ParseListText(it->second))
    list << QString::fromStdString(item);
  return list;
}

QString HomeShort(const QString& path) {
  const QString home = QDir::homePath();
  return path.startsWith(home + "/") ? "~" + path.mid(home.size()) : path;
}

QString Expanded(const QString& path) {
  return path.startsWith("~/") ? QDir::homePath() + path.mid(1) : path;
}

}  // namespace

void SettingsPanel::AddMiraFoldersCard(SettingsPage* page) {
  mira_folders_card_ = page->AddCard("Mira folders");
}

void SettingsPanel::ShowMiraFolders(const ConfigResult& config) {
  if (mira_folders_card_ == nullptr) return;
  mira_folders_card_->ClearRows();
  const QStringList folders = ListValue(config, "mira_folders");
  for (const QString& folder : folders) {
    const QStorageInfo drive(Expanded(folder));
    const QString space = drive.isValid()
                              ? QString("%1 GB free").arg(drive.bytesAvailable() / 1'000'000'000)
                              : QString("Not there: its disk isn't mounted");
    auto* row = new SettingRow(HomeShort(Expanded(folder)), space, mira_folders_card_);
    if (folders.size() > 1) {
      auto* remove = new QPushButton("Remove", row);
      connect(remove, &QPushButton::clicked, this, [this, folder] {
        if (!notify::Confirm(this, "Remove this Mira folder?",
                             "Its games and apps leave the library. Nothing is deleted from the "
                             "disk, and adding the folder again brings them back.",
                             "Remove")) {
          return;
        }
        EditMiraFolders([folder](QStringList& folders, QStringList& roots) {
          folders.removeAll(folder);
          roots.removeIf([&](const QString& root) {
            return Expanded(root).startsWith(Expanded(folder) + "/");
          });
        });
      });
      row->AddControl(remove);
    }
    mira_folders_card_->AddRow(row);
  }
  auto* add_row = new SettingRow(
      "Add a Mira folder",
      "Pick a folder on another disk: Mira makes Games and Applications in it and watches it. "
      "Drop a game, app, ROM or archive into it and Mira sorts it into place.",
      mira_folders_card_);
  auto* add = new QPushButton("Add…", add_row);
  connect(add, &QPushButton::clicked, this, [this] {
    const QString folder = QFileDialog::getExistingDirectory(this, "Choose a Mira folder");
    if (folder.isEmpty()) return;
    EditMiraFolders([folder](QStringList& folders, QStringList& roots) {
      if (std::ranges::any_of(folders, [&](const QString& f) { return Expanded(f) == folder; }))
        return;
      folders << folder;
      for (const QString& sub : {"/Games", "/Applications"}) {
        if (!roots.contains(folder + sub)) roots << folder + sub;
      }
    });
  });
  add_row->AddControl(add);
  mira_folders_card_->AddRow(add_row);
  nav_->RegisterRow(add_row, "mira folder disk drive storage library add second");
}

void SettingsPanel::EditMiraFolders(
    const std::function<void(QStringList& folders, QStringList& roots)>& edit) {
  // From the settings as saved now: Library folders may have been saved since this page loaded.
  api::GetConfigAsync(this, [this, edit](ConfigResult config) {
    if (!config.ok) {
      notify::FailedRequest(this, "Could not change the Mira folders.", config.error);
      return;
    }
    QStringList folders = ListValue(config, "mira_folders");
    QStringList roots = ListValue(config, "library_roots");
    edit(folders, roots);
    SaveMiraFolders(folders, roots);
  });
}

void SettingsPanel::SaveMiraFolders(const QStringList& folders, const QStringList& roots) {
  const auto json = [](const QStringList& list) {
    return QJsonDocument(QJsonArray::fromStringList(list))
        .toJson(QJsonDocument::Compact)
        .toStdString();
  };
  const std::vector<ConfigEdit> edits = {
      {"mira_folders", "an array of strings", json(folders)},
      {"library_roots", "an array of strings", json(roots)},
  };
  api::PatchConfigAsync(this, edits, [this](PatchConfigResult result) {
    if (!result.ok) {
      notify::FailedRequest(this, "Could not change the Mira folders.", result.error);
      return;
    }
    api::GetConfigAsync(this, [this](ConfigResult config) {
      if (!config.ok) return;
      ShowMiraFolders(config);
      // Library folders show the roots just added or removed.
      for (SettingEditor& field : fields_) {
        if (field.entry.key != "library_roots") continue;
        const auto it = config.values.find(field.entry.key);
        if (it == config.values.end()) continue;
        // An edit not yet saved stays, as a change from the new value.
        const std::optional<std::string> edit =
            field.Changed() ? std::optional(field.Text()) : std::nullopt;
        field.SetText(it->second);
        field.original = field.Text();
        if (edit) field.SetText(*edit);
      }
      Refresh();
    });
  });
}

}  // namespace mira_gui
