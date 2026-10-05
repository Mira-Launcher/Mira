#include "SettingsPanel.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <utility>

#include "../client/EventHub.h"
#include "AppearancePreviews.h"
#include "../app/ErrorHelp.h"
#include "../app/KeyBindings.h"
#include "../app/Notify.h"
#include "../sources/Sources.h"
#include "../theme/Theme.h"
#include "../widgets/ShortcutEdit.h"
#include "SettingsCard.h"
#include "SettingsNav.h"

namespace mira_gui {
namespace {

// The card each shortcut sits in on the Shortcuts page.
QString ShortcutGroup(const QString& id) {
  static const QHash<QString, QString> kGroups = {
      {"focus_search", "Library"},       {"clear_or_deselect", "Library"},    {"toggle_hidden", "Library"},
      {"refresh", "Library"},            {"zoom_in", "Tiles"},                {"zoom_out", "Tiles"},
      {"reset_zoom", "Tiles"},           {"play_stop", "Selected game"},      {"details_settings", "Selected game"},
      {"delete_game", "Selected game"},
  };
  return kGroups.value(id, "Window");
}

// The games the theme and layout previews draw: recently played first, then pinned.
std::vector<GameSummary> PreviewGames(const SettingsPanel::Previews& previews) {
  std::vector<GameSummary> games;
  for (const auto* list : {&previews.recent, &previews.pinned}) {
    for (const GameSummary& game : *list) {
      if (std::ranges::find(games, game.id, &GameSummary::id) == games.end()) games.push_back(game);
    }
  }
  if (games.size() > 8) games.resize(8);
  return games;
}

// Two games for the tile preview, the second from a store when one is at
// hand, so the source mark has something to draw.
std::vector<GameSummary> TilePreviewGames(const std::vector<GameSummary>& games) {
  std::vector<GameSummary> picked;
  if (games.empty()) return picked;
  picked.push_back(games.front());
  const auto from_source = std::ranges::find_if(games.begin() + 1, games.end(), [](const GameSummary& game) {
    return FindSourceInfo(QString::fromStdString(game.source)) != nullptr;
  });
  if (from_source != games.end()) {
    picked.push_back(*from_source);
  } else if (games.size() > 1) {
    picked.push_back(games[1]);
  }
  return picked;
}

}  // namespace

SettingsPanel::SettingsPanel(Previews previews, QWidget* parent) : QWidget(parent), previews_(std::move(previews)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  nav_ = new SettingsNavWidget(this);
  layout->addWidget(nav_, /*stretch=*/1);

  change_bar_ = new ChangeBar(nav_->ContentArea());
  connect(change_bar_, &ChangeBar::DiscardClicked, this, &SettingsPanel::DiscardChanges);
  connect(change_bar_, &ChangeBar::SaveClicked, this, &SettingsPanel::Save);

  BuildInterfacePage();
  BuildSidebarPage();
  BuildShortcutsPage();

  setEnabled(false);
  LoadFrontendPrefs();
  Load();
}

void SettingsPanel::SetHeader(QWidget* header) { nav_->SetHeaderWidget(header); }

// --- Frontend pages -------------------------------------------------------

size_t SettingsPanel::AddPrefField(PrefField field) {
  if (field.row != nullptr) {
    connect(field.row, &SettingRow::RevertClicked, this, [this, revert = field.revert] {
      revert();
      Refresh();
    });
  }
  pref_fields_.push_back(std::move(field));
  return pref_fields_.size() - 1;
}

void SettingsPanel::AddCardReset(SettingsCard* card, std::vector<size_t> prefs,
                                 std::vector<size_t> schema) {
  connect(card, &SettingsCard::ResetClicked, this, [this, prefs, schema] {
    for (const size_t i : prefs) pref_fields_[i].reset();
    for (const size_t i : schema) fields_[i].SetText(fields_[i].entry.default_display);
    Refresh();
  });
  card_resets_.push_back({card, std::move(prefs), std::move(schema)});
}

Switch* SettingsPanel::AddToggle(SettingsCard* card, const QString& label, const QString& doc, const QString& search,
                                 std::optional<bool> FrontendPrefs::*member, bool fallback) {
  auto* row = new SettingRow(label, doc);
  auto* toggle = new Switch(row);
  toggle->setAccessibleName(label);
  toggle->setChecked(fallback);
  row->AddControl(toggle);
  card->AddRow(row);
  nav_->RegisterRow(row, label + ' ' + search);
  toggles_.push_back({toggle, member, fallback, fallback});
  const size_t index = toggles_.size() - 1;
  connect(toggle, &Switch::toggled, this, &SettingsPanel::Refresh);
  AddPrefField({.row = row,
                .changed = [this, index] { return toggles_[index].toggle->isChecked() != toggles_[index].saved; },
                .is_default = [this, index] { return toggles_[index].toggle->isChecked() == toggles_[index].fallback; },
                .revert = [this, index] { toggles_[index].toggle->setChecked(toggles_[index].saved); },
                .reset = [this, index] { toggles_[index].toggle->setChecked(toggles_[index].fallback); },
                .mark_saved = [this, index] { toggles_[index].saved = toggles_[index].toggle->isChecked(); }});
  return toggle;
}

void SettingsPanel::BuildInterfacePage() {
  SettingsPage* page = nav_->AddCategory("Interface", CategoryGlyph("Interface"), CategoryNavGroup("Interface"));
  pages_["Interface"] = page;
  const std::vector<GameSummary> games = PreviewGames(previews_);

  SettingsCard* appearance = page->AddCard("Appearance");
  auto* theme_row = new SettingRow("Theme",
                                   "To add a theme, put a .toml file in ~/.config/mira/themes. The bundled "
                                   "themes show which keys you can set.");
  auto* tiles = new QWidget(theme_row);
  auto* tiles_layout = new QHBoxLayout(tiles);
  tiles_layout->setContentsMargins(0, 0, 0, 4);
  tiles_layout->setSpacing(10);
  themes_ = new QButtonGroup(this);
  QStringList names{"auto"};
  names << theme::Available();
  for (const QString& name : names) {
    auto* choice = new ThemeChoice(name, name == "auto" ? QString("Follow the desktop") : name, games,
                                   previews_.artwork, tiles);
    themes_->addButton(choice);
    tiles_layout->addWidget(choice);
  }
  tiles_layout->addStretch(1);
  theme_row->SetBelow(tiles);
  appearance->AddRow(theme_row);
  nav_->RegisterRow(theme_row, "theme appearance dark light colors");
  connect(themes_, &QButtonGroup::buttonClicked, this, &SettingsPanel::Refresh);
  AddPrefField({.row = theme_row,
                .changed = [this] { return SelectedTheme() != theme_saved_; },
                .is_default = [this] { return SelectedTheme() == "auto"; },
                .revert = [this] { SelectTheme(theme_saved_); },
                .reset = [this] { SelectTheme("auto"); },
                .mark_saved = [this] { theme_saved_ = SelectedTheme(); }});

  SettingsCard* general = page->AddCard("General");
  AddToggle(general, "Scan the library on startup",
            "Scan the library each time Mira opens. The daemon already watches your folders while it runs, so "
            "this only catches changes made while it was stopped.",
            "scan startup", &FrontendPrefs::scan_on_startup, true);
  AddToggle(general, "Drag to select games", "Drag across the library to select several games at once.",
            "rubber band multiple", &FrontendPrefs::drag_select, true);

  SettingsCard* library = page->AddCard("Library");
  tile_preview_ = new TilePreview(TilePreviewGames(games), previews_.artwork);
  auto* preview_holder = new QWidget();
  auto* preview_layout = new QHBoxLayout(preview_holder);
  preview_layout->setContentsMargins(18, 4, 18, 10);
  preview_layout->addWidget(tile_preview_);
  preview_layout->addStretch(1);
  library->AddRow(preview_holder);
  AddToggle(library, "Filter tabs", "Tabs above the grid for All, Installed, Playing now and the other filters.",
            "library chips", &FrontendPrefs::library_filter_tabs, true);
  continue_row_ = AddToggle(library, "Continue playing",
                            "Large cards for running and recently played games above the grid.",
                            "library cards recently played recent", &FrontendPrefs::library_continue_row, true);
  continue_count_row_ = new SettingRow("Cards to show", "How many cards the Continue playing row shows.");
  continue_count_ = new QSpinBox(continue_count_row_);
  continue_count_->setRange(1, 6);
  continue_count_->setValue(3);
  continue_count_->setMinimumWidth(90);
  continue_count_row_->AddControl(continue_count_);
  library->AddRow(continue_count_row_);
  nav_->RegisterRow(continue_count_row_, "continue playing cards count library");
  connect(continue_count_, &QSpinBox::valueChanged, this, &SettingsPanel::Refresh);
  AddPrefField({.row = continue_count_row_,
                .changed = [this] { return continue_count_->value() != continue_count_saved_; },
                .is_default = [this] { return continue_count_->value() == 3; },
                .revert = [this] { continue_count_->setValue(continue_count_saved_); },
                .reset = [this] { continue_count_->setValue(3); },
                .mark_saved = [this] { continue_count_saved_ = continue_count_->value(); }});
  tile_status_ = AddToggle(library, "Status on tiles", "Show Needs install, Broken, Playing and the like on a tile.",
                           "tile badge", &FrontendPrefs::tile_status, true);
  tile_mark_ = AddToggle(library, "Source mark on tiles", "Show which store or launcher a game came from on its tile.",
                         "tile store icon", &FrontendPrefs::tile_source_mark, true);
  tile_pin_ = AddToggle(library, "Pin badge on tiles", "Show a pin in the corner of a pinned game's tile.",
                        "tile pinned favorite icon", &FrontendPrefs::tile_pin_badge, true);
  AddToggle(library, "Same tile size everywhere",
            "One tile size for the library and every source page. Off, each page keeps its own.",
            "tile size zoom synced source pages", &FrontendPrefs::tile_size_synced, false);
  AddToggle(library, "Tabs on source pages",
            "Split a source's games into Installed and Not installed tabs. Off lists both, one above the other.",
            "source page installed not installed", &FrontendPrefs::source_page_tabs, true);

  SettingsCard* layout = page->AddCard("Layout");
  layout_preview_ = new LayoutPreview(games, previews_.artwork);
  auto* layout_holder = new QWidget();
  auto* layout_holder_layout = new QHBoxLayout(layout_holder);
  layout_holder_layout->setContentsMargins(18, 4, 18, 10);
  layout_holder_layout->addWidget(layout_preview_);
  layout->AddRow(layout_holder);
  std::vector<size_t> layout_fields;
  const auto shape_row = [&](ShapeField& field, const QString& label, const QString& doc, int maximum,
                             std::optional<int> FrontendPrefs::*member) {
    field.member = member;
    auto* row = new SettingRow(label, doc);
    row->AddControl(MakeSlider(field, maximum));
    layout->AddRow(row);
    nav_->RegisterRow(row, label + " layout corner radius spacing");
    ShapeField* shape = &field;
    layout_fields.push_back(AddPrefField({.row = row,
                  .changed = [shape] { return shape->current != shape->saved; },
                  .is_default = [shape] { return !shape->current.has_value(); },
                  .revert = [this, shape] { shape->current = shape->saved; RefreshShape(*shape); },
                  .reset = [this, shape] { shape->current.reset(); RefreshShape(*shape); },
                  .mark_saved = [shape] { shape->saved = shape->current; }}));
  };
  shape_row(tile_spacing_, "Tile gap",
            "Empty space around each tile. The zoom slider in the top bar changes the tile size.", 40,
            &FrontendPrefs::tile_spacing);
  shape_row(grid_margin_, "Grid padding", "Space between the grid and the window edges and side panels.", 60,
            &FrontendPrefs::grid_margin);
  shape_row(tile_radius_, "Cover rounding", "Corner radius of game covers. 0 is square.", 40,
            &FrontendPrefs::tile_radius);
  shape_row(panel_radius_, "Panel rounding", "Corner radius of panels, cards and notices.", 24,
            &FrontendPrefs::panel_radius);
  shape_row(control_radius_, "Control rounding", "Corner radius of buttons, fields and dropdowns.", 20,
            &FrontendPrefs::control_radius);
  AddCardReset(layout, std::move(layout_fields), {});
  RefreshShapeDefaults();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, &SettingsPanel::RefreshShapeDefaults);
  UpdatePreviews();
}

QWidget* SettingsPanel::MakeSlider(ShapeField& field, int maximum) {
  auto* box = new QWidget();
  auto* layout = new QHBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);
  field.slider = new QSlider(Qt::Horizontal, box);
  field.slider->setRange(0, maximum);
  field.slider->setFixedWidth(180);
  layout->addWidget(field.slider);
  field.value = new QLabel(box);
  field.value->setProperty("role", "subtle");
  field.value->setMinimumWidth(130);
  layout->addWidget(field.value);
  ShapeField* shape = &field;
  connect(field.slider, &QSlider::valueChanged, this, [this, shape](int value) {
    shape->current = value;
    RefreshShape(*shape);
    Refresh();
  });
  return box;
}

void SettingsPanel::RefreshShape(ShapeField& field) {
  const int value = field.current.value_or(field.theme_default);
  {
    const QSignalBlocker block(field.slider);
    field.slider->setValue(value);
  }
  field.value->setText(field.current ? QString("%1 px").arg(value) : QString("%1 px, theme default").arg(value));
  UpdatePreviews();
}

void SettingsPanel::RefreshShapeDefaults() {
  const theme::Tokens& defaults = theme::ThemeDefaults();
  tile_spacing_.theme_default = defaults.tile_spacing;
  grid_margin_.theme_default = defaults.grid_margin;
  tile_radius_.theme_default = defaults.radius_tile;
  panel_radius_.theme_default = defaults.radius_panel;
  control_radius_.theme_default = defaults.radius_control;
  for (ShapeField* field : {&tile_spacing_, &grid_margin_, &tile_radius_, &panel_radius_, &control_radius_}) {
    RefreshShape(*field);
  }
}

void SettingsPanel::UpdatePreviews() {
  if (tile_preview_ != nullptr && tile_status_ != nullptr && tile_mark_ != nullptr && tile_pin_ != nullptr) {
    tile_preview_->SetShown(tile_status_->isChecked(), tile_mark_->isChecked(), tile_pin_->isChecked());
  }
  if (layout_preview_ != nullptr) {
    layout_preview_->SetShape({tile_spacing_.current.value_or(tile_spacing_.theme_default),
                               grid_margin_.current.value_or(grid_margin_.theme_default),
                               tile_radius_.current.value_or(tile_radius_.theme_default),
                               panel_radius_.current.value_or(panel_radius_.theme_default),
                               control_radius_.current.value_or(control_radius_.theme_default)});
  }
  if (continue_count_row_ != nullptr && continue_row_ != nullptr) {
    continue_count_row_->setEnabled(continue_row_->isChecked());
  }
}

QString SettingsPanel::SelectedTheme() const {
  const auto* choice = qobject_cast<ThemeChoice*>(themes_->checkedButton());
  return choice != nullptr ? choice->Theme() : QString("auto");
}

void SettingsPanel::SelectTheme(const QString& name) {
  for (QAbstractButton* button : themes_->buttons()) {
    if (static_cast<ThemeChoice*>(button)->Theme() == name) button->setChecked(true);
  }
}

void SettingsPanel::BuildSidebarPage() {
  SettingsPage* page = nav_->AddCategory("Sidebar", CategoryGlyph("Sidebar"), CategoryNavGroup("Sidebar"));
  pages_["Sidebar"] = page;

  SettingsCard* style = page->AddCard("Pinned and recently played");
  sidebar_style_ = new SidebarStyleChoices({}, previews_.pinned, previews_.recent, previews_.artwork, this);
  const QStringList searches = {"sidebar pinned style look covers hero banners shelf",
                                "sidebar recently played recent style look covers hero banners shelf",
                                "sidebar recently played when last played date time ago",
                                "sidebar recently played recent games count"};
  const QList<SettingRow*> rows = sidebar_style_->Rows();
  for (int i = 0; i < rows.size(); ++i) {
    style->AddRow(rows[i]);
    nav_->RegisterRow(rows[i], rows[i]->Label()->text() + ' ' + searches.value(i));
  }
  connect(sidebar_style_, &SidebarStyleChoices::Changed, this, &SettingsPanel::Refresh);
  // One field per choice, so each row is marked and counted on its own.
  using Choices = SidebarStyleChoices::Choices;
  const auto choice_field = [this](SettingRow* row, auto member) {
    PrefField field;
    field.row = row;
    field.changed = [this, member] { return sidebar_style_->Current().*member != sidebar_style_saved_.*member; };
    field.is_default = [this, member] { return sidebar_style_->Current().*member == Choices{}.*member; };
    field.revert = [this, member] {
      Choices choices = sidebar_style_->Current();
      choices.*member = sidebar_style_saved_.*member;
      sidebar_style_->SetChoices(choices);
    };
    field.reset = [this, member] {
      Choices choices = sidebar_style_->Current();
      choices.*member = Choices{}.*member;
      sidebar_style_->SetChoices(choices);
    };
    field.mark_saved = [this, member] { sidebar_style_saved_.*member = sidebar_style_->Current().*member; };
    AddPrefField(std::move(field));
  };
  choice_field(rows[0], &Choices::pinned);
  choice_field(rows[1], &Choices::recent);
  choice_field(rows[2], &Choices::recent_when);
  choice_field(rows[3], &Choices::recent_count);

  sources_card_ = page->AddCard("Sources");
  AddToggle(sources_card_, "Show game counts", "Show how many games each source has next to its name.",
            "sidebar source numbers", &FrontendPrefs::sidebar_source_counts, true);
  AddToggle(sources_card_, "Colored source icons", "Show each source's colored initial instead of a dot.",
            "sidebar source colors", &FrontendPrefs::sidebar_source_icons, true);
  for (const SourceInfo& source : AllSources()) {
    auto* row = new SettingRow(source.name, {});
    row->ShowGrip();
    row->SetLeading(MakeSourceBadge(source, 20, row));
    row->AddAfterLabel(MakeKindTag(source, row));
    auto* shown = new Switch(row);
    shown->setChecked(true);
    shown->setAccessibleName(QString("Show %1 in the sidebar").arg(source.name));
    shown->setToolTip("Show in the sidebar");
    row->AddControl(shown);
    sources_card_->AddRow(row);
    nav_->RegisterRow(row, QString("sidebar show source order %1").arg(source.name));
    source_rows_.push_back({source.id, row, shown});
    const size_t index = source_rows_.size() - 1;
    connect(shown, &Switch::toggled, this, &SettingsPanel::Refresh);
    AddPrefField({.row = row,
                  .changed = [this, index] {
                    return source_rows_[index].shown->isChecked() ==
                           hidden_sources_saved_.contains(source_rows_[index].id);
                  },
                  .is_default = [this, index] { return source_rows_[index].shown->isChecked(); },
                  .revert = [this, index] {
                    source_rows_[index].shown->setChecked(!hidden_sources_saved_.contains(source_rows_[index].id));
                  },
                  .reset = [this, index] { source_rows_[index].shown->setChecked(true); },
                  .mark_saved = [] {}});
  }
  // The order is one change however many rows a drag moves. The order shown
  // is the baseline until the saved prefs arrive, so a failed fetch isn't a change.
  source_order_saved_ = CurrentSourceOrder();
  pref_fields_.push_back({.row = nullptr,
                          .changed = [this] { return CurrentSourceOrder() != source_order_saved_; },
                          .is_default = [] { return true; },
                          .revert = [this] { ArrangeSources(source_order_saved_); },
                          .reset = [] {},
                          .mark_saved = [] {}});
  connect(sources_card_, &SettingsCard::RowsReordered, this, &SettingsPanel::Refresh);
}

QStringList SettingsPanel::CurrentSourceOrder() const {
  QStringList order;
  for (QWidget* row : sources_card_->Rows()) {
    const auto it = std::ranges::find(source_rows_, row, [](const SourceRow& source) -> QWidget* { return source.row; });
    if (it != source_rows_.end()) order << it->id;
  }
  return order;
}

QSet<QString> SettingsPanel::CurrentHiddenSources() const {
  QSet<QString> hidden;
  for (const SourceRow& row : source_rows_) {
    if (!row.shown->isChecked()) hidden.insert(row.id);
  }
  return hidden;
}

void SettingsPanel::ArrangeSources(const QStringList& order) {
  // Sources missing from the saved order follow it, in their usual order.
  QStringList full = order;
  for (const SourceRow& row : source_rows_) {
    if (!full.contains(row.id)) full << row.id;
  }
  const int first = static_cast<int>(sources_card_->Rows().size() - source_rows_.size());
  int at = first;
  for (const QString& id : full) {
    const auto it = std::ranges::find(source_rows_, id, &SourceRow::id);
    if (it == source_rows_.end()) continue;
    sources_card_->MoveRow(it->row, at++);
  }
}

void SettingsPanel::BuildShortcutsPage() {
  // The owning window registered every shortcut and loaded its overrides
  // before this screen could open, so the registry already holds current state.
  SettingsPage* page = nav_->AddCategory("Shortcuts", CategoryGlyph("Shortcuts"), CategoryNavGroup("Shortcuts"));
  pages_["Shortcuts"] = page;
  std::vector<std::pair<SettingsCard*, std::vector<size_t>>> cards;  // in the order first seen
  for (const keybindings::Binding& binding : keybindings::All()) {
    const QString group = ShortcutGroup(binding.id);
    auto in_card =
        std::ranges::find(cards, group, [](const auto& entry) { return entry.first->Title(); });
    if (in_card == cards.end()) {
      cards.emplace_back(page->AddCard(group), std::vector<size_t>{});
      in_card = cards.end() - 1;
    }
    SettingsCard* card = in_card->first;

    const QKeySequence current = keybindings::Override(binding.id).value_or(binding.default_keys);
    auto* row = new SettingRow(binding.label, {});
    if (!binding.extra_aliases.isEmpty()) {
      QStringList aliases;
      for (const QKeySequence& alias : binding.extra_aliases) aliases << alias.toString(QKeySequence::NativeText);
      auto* also = new QLabel("also " + aliases.join(", "), row);
      also->setProperty("role", "subtle");
      also->setToolTip("Always works too and can't be changed.");
      row->AddControl(also);
    }
    auto* edit = new ShortcutEdit(current, row);
    edit->setAccessibleName(binding.label);
    row->AddControl(edit);
    card->AddRow(row);
    nav_->RegisterRow(row, QString("%1 shortcut keyboard keys").arg(binding.label));

    shortcuts_.push_back({binding.id, edit, current, binding.default_keys});
    const size_t index = shortcuts_.size() - 1;
    connect(edit, &ShortcutEdit::Changed, this, &SettingsPanel::Refresh);
    in_card->second.push_back(
        AddPrefField({.row = row,
                      .changed = [this, index] {
                        return shortcuts_[index].edit->Keys() != shortcuts_[index].saved;
                      },
                      .is_default = [this, index] {
                        return shortcuts_[index].edit->Keys() == shortcuts_[index].default_keys;
                      },
                      .revert = [this, index] {
                        shortcuts_[index].edit->SetKeys(shortcuts_[index].saved);
                      },
                      .reset = [this, index] {
                        shortcuts_[index].edit->SetKeys(shortcuts_[index].default_keys);
                      },
                      .mark_saved = [this, index] {
                        shortcuts_[index].saved = shortcuts_[index].edit->Keys();
                      }}));
  }
  for (auto& [card, fields] : cards) AddCardReset(card, std::move(fields), {});
}

void SettingsPanel::LoadFrontendPrefs() {
  theme_saved_ = theme::CurrentName();
  SelectTheme(theme_saved_);

  MiradClient::GetFrontendPrefsAsync(this, [this](FrontendPrefsResult result) {
    if (!result.ok) return;  // the defaults are already shown
    const FrontendPrefs& prefs = result.prefs;
    if (prefs.theme) {
      theme_saved_ = QString::fromStdString(*prefs.theme);
      SelectTheme(theme_saved_);
    }
    for (PrefToggle& toggle : toggles_) {
      toggle.saved = (prefs.*toggle.member).value_or(toggle.fallback);
      toggle.toggle->setChecked(toggle.saved);
    }
    continue_count_->setValue(prefs.library_continue_count.value_or(3));
    continue_count_saved_ = continue_count_->value();  // after the clamp
    for (ShapeField* field : {&tile_spacing_, &grid_margin_, &tile_radius_, &panel_radius_, &control_radius_}) {
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

    hidden_sources_saved_.clear();
    for (const std::string& id : prefs.hidden_sources.value_or(std::vector<std::string>{})) {
      hidden_sources_saved_.insert(QString::fromStdString(id));
    }
    for (const SourceRow& row : source_rows_) row.shown->setChecked(!hidden_sources_saved_.contains(row.id));
    QStringList order;
    for (const std::string& id : prefs.source_order.value_or(std::vector<std::string>{})) {
      order << QString::fromStdString(id);
    }
    ArrangeSources(order);
    source_order_saved_ = CurrentSourceOrder();
    Refresh();
  });
}

// --- Schema pages ---------------------------------------------------------

void SettingsPanel::Load() {
  MiradClient::GetConfigSchemaAsync(this, [this](ConfigSchemaResult schema) {
    if (!schema.ok) {
      emit LoadFailed(error_help::Describe(schema.error));
      return;
    }
    for (ConfigSchemaEntry& entry : schema.entries) {
      if (entry.game_only) continue;
      SettingEditor field;
      field.entry = std::move(entry);
      fields_.push_back(std::move(field));
    }
    BuildSchemaPages();

    const auto list_runners = [this] {
      MiradClient::ListRunnersAsync(this, [this](RunnersResult result) { PopulateRunnerCombos(result); });
    };
    list_runners();
    connect(EventHub::Instance(), &EventHub::RunnersChanged, this, list_runners);

    MiradClient::GetConfigAsync(this, [this](ConfigResult config) {
      if (!config.ok) {
        emit LoadFailed(error_help::Describe(config.error));
        return;
      }
      for (SettingEditor& field : fields_) {
        const auto it = config.values.find(field.entry.key);
        field.original = it != config.values.end() ? it->second : field.entry.default_display;
        field.SetText(field.original);
        field.original = field.Text();  // as the editor holds it (a clamped number, a joined list)
      }
      nav_->RearrangePages();  // lists and paths now hold their rows, so cards have their real heights
      setEnabled(true);
      Refresh();
      if (!pending_focus_key_.isEmpty()) FocusKey(std::exchange(pending_focus_key_, QString()));
    });
  });
}

void SettingsPanel::BuildSchemaPages() {
  std::vector<std::string> categories;
  for (const SettingEditor& field : fields_) categories.push_back(field.entry.category);

  for (const auto& [category, rows] : GroupByCategory(categories)) {
    SettingsPage* page = nav_->AddCategory(category, CategoryGlyph(category), CategoryNavGroup(category));
    pages_[category] = page;
    const bool by_source = std::ranges::all_of(rows, [this](size_t i) { return !fields_[i].entry.source.empty(); });
    if (by_source) {
      BuildSourceCards(page, rows);
      continue;
    }
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
    if (std::ranges::find(sources, fields_[i].entry.source) == sources.end()) sources.push_back(fields_[i].entry.source);
  }
  for (const std::string& source_id : sources) {
    const QString id = QString::fromStdString(source_id);
    const SourceInfo* source = FindSourceInfo(id);
    auto* card = new SettingsCard(source != nullptr ? source->name : id);
    page->AddWidget(card);
    if (source != nullptr) card->SetLeading(MakeSourceBadge(*source, 24, card));
    for (const size_t i : rows) {
      if (fields_[i].entry.source != source_id) continue;
      if (fields_[i].entry.key == source_id + ".enabled") {
        // The source's on switch sits in its card's header, not in a row of its own.
        SettingRow* row = fields_[i].Build(card);
        row->hide();
        fields_[i].OnEdited(this, [this] { Refresh(); });
        fields_[i].toggle->setToolTip(QString::fromStdString(fields_[i].entry.label));
        card->Header()->addWidget(fields_[i].toggle);
        continue;
      }
      AddSchemaRow(card, i);
    }
    card->SetCollapsible(/*collapsed=*/true);
  }
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
  const std::string wanted = key.toStdString();
  const auto it = std::ranges::find(fields_, wanted, [](const SettingEditor& f) { return f.entry.key; });
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

void SettingsPanel::AddSectionAction(const QString& category, const QString& card, const QString& label,
                                     const QString& doc, const QString& button_text, std::function<void()> activated) {
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
  MiradClient::GetGameModeStatusAsync(this, [this](GameModeStatusResult result) {
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
  for (const SettingEditor& field : fields_) count += field.row != nullptr && field.Changed() ? 1 : 0;
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
  // A dragged source row is marked too, though the order counts as one change.
  if (!source_order_saved_.isEmpty()) {
    const QStringList order = CurrentSourceOrder();
    for (const SourceRow& row : source_rows_) {
      if (order.indexOf(row.id) != source_order_saved_.indexOf(row.id)) row.row->SetModified(true);
    }
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
  change_bar_->SetBusy(true);
  save_error_.clear();

  if (PrefsDirty()) {
    FrontendPrefs prefs;
    for (const PrefToggle& toggle : toggles_) prefs.*toggle.member = toggle.toggle->isChecked();
    prefs.library_continue_count = continue_count_->value();
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
    if (shapes_changed || theme_name != theme_saved_) theme::Configure(theme_name, overrides);  // one restyle

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

    // Only when changed here: the sidebar edits both too while Settings is open.
    if (CurrentHiddenSources() != hidden_sources_saved_) {
      std::vector<std::string> hidden;
      for (const QString& id : CurrentHiddenSources()) hidden.push_back(id.toStdString());
      std::ranges::sort(hidden);
      prefs.hidden_sources = std::move(hidden);
    }
    if (CurrentSourceOrder() != source_order_saved_) {
      std::vector<std::string> order;
      for (const QString& id : CurrentSourceOrder()) order.push_back(id.toStdString());
      prefs.source_order = std::move(order);
    }

    for (const PrefField& field : pref_fields_) field.mark_saved();
    hidden_sources_saved_ = CurrentHiddenSources();
    source_order_saved_ = CurrentSourceOrder();
    MiradClient::SaveFrontendPrefsAsync(this, prefs, [this](PatchConfigResult result) {
      if (!result.ok) notify::FailedRequest(this, "Could not save the display settings.", result.error);
    });
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
  saves_pending_ = (edits.empty() ? 0 : 1) + static_cast<int>(resets.size());
  if (saves_pending_ == 0) {
    FinishSave(true, {});
    return;
  }
  const auto done = [this](bool ok, const std::string& error, const std::vector<size_t>& indices) {
    if (ok) {
      for (const size_t i : indices) fields_[i].original = fields_[i].Text();
    } else if (save_error_.isEmpty()) {
      save_error_ = QString::fromStdString(error);
    }
    if (--saves_pending_ == 0) FinishSave(save_error_.isEmpty(), save_error_);
  };
  if (!edits.empty()) {
    MiradClient::PatchConfigAsync(this, edits, [done, edited](PatchConfigResult result) {
      done(result.ok, result.error, edited);
    });
  }
  for (const size_t i : resets) {
    MiradClient::ResetConfigKeyAsync(this, fields_[i].entry.key, [done, i](PatchConfigResult result) {
      done(result.ok, result.error, {i});
    });
  }
}

void SettingsPanel::FinishSave(bool ok, const QString& error) {
  change_bar_->SetBusy(false);
  Refresh();
  emit SaveFinished(ok, error);
}

}  // namespace mira_gui
