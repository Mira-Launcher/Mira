// SettingsPanel's own pages, the ones not built from mirad's schema:
// Interface, Sidebar and Shortcuts, and the pref fields they register.
#include "SettingsPanel.h"

#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>

#include <algorithm>
#include <optional>
#include <utility>

#include "../app/KeyBindings.h"
#include "../sources/Sources.h"
#include "../theme/Theme.h"
#include "../widgets/ShortcutEdit.h"
#include "AppearancePreviews.h"
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

QDoubleSpinBox* SettingsPanel::AddSeconds(SettingsCard* card, const QString& label, const QString& doc,
                                          const QString& search, std::optional<int> FrontendPrefs::*member,
                                          int fallback_ms, double maximum) {
  auto* row = new SettingRow(label, doc);
  auto* spin = new QDoubleSpinBox(row);
  spin->setRange(0, maximum);
  spin->setSingleStep(0.5);
  spin->setDecimals(1);
  spin->setSuffix(" s");
  spin->setMinimumWidth(90);
  spin->setValue(fallback_ms / 1000.0);
  row->AddControl(spin);
  card->AddRow(row);
  nav_->RegisterRow(row, label + ' ' + search);
  seconds_.push_back({spin, member, fallback_ms, fallback_ms});
  const size_t index = seconds_.size() - 1;
  connect(spin, &QDoubleSpinBox::valueChanged, this, &SettingsPanel::Refresh);
  AddPrefField({.row = row,
                .changed = [this, index] { return qRound(seconds_[index].spin->value() * 1000) != seconds_[index].saved; },
                .is_default = [this, index] { return qRound(seconds_[index].spin->value() * 1000) == seconds_[index].fallback; },
                .revert = [this, index] { seconds_[index].spin->setValue(seconds_[index].saved / 1000.0); },
                .reset = [this, index] { seconds_[index].spin->setValue(seconds_[index].fallback / 1000.0); },
                .mark_saved = [this, index] { seconds_[index].saved = qRound(seconds_[index].spin->value() * 1000); }});
  return spin;
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
  AddToggle(general, "Double-click a game to play it",
            "Double-clicking a game in the library or on a source page starts it, or stops it while it runs.",
            "double click launch start play", &FrontendPrefs::double_click_play, true);

  SettingsCard* library = page->AddCard("Library");
  tile_preview_ = new TilePreview(TilePreviewGames(games), previews_.artwork);
  auto* preview_holder = new QWidget();
  auto* preview_layout = new QHBoxLayout(preview_holder);
  preview_layout->setContentsMargins(18, 4, 18, 10);
  preview_layout->addWidget(tile_preview_);
  preview_layout->addStretch(1);
  library->AddRow(preview_holder);
  AddToggle(library, "Filter tabs", "Tabs above the grid for All, Games, Apps, Installed and the other filters.",
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
  AddToggle(library, "Apps under All", "List apps under the All tab too, not only under Apps.",
            "library apps all tab filter", &FrontendPrefs::library_apps_in_all, true);
  AddToggle(library, "Apps in Continue playing",
            "Show recently used apps in the Continue playing row, not only games.",
            "library cards recently played recent apps", &FrontendPrefs::library_continue_apps, false);
  tile_status_ = AddToggle(library, "Status on tiles", "Show Needs install, Broken, Playing and the like on a tile.",
                           "tile badge", &FrontendPrefs::tile_status, true);
  tile_source_ = AddToggle(library, "Source on tiles",
                           "Name the store or launcher a game came from under its title.",
                           "tile store launcher source", &FrontendPrefs::tile_source_mark, true);
  AddToggle(library, "Same tile size everywhere",
            "One tile size for the library and every source page. Off, each page keeps its own.",
            "tile size zoom synced source pages", &FrontendPrefs::tile_size_synced, false);

  SettingsCard* trailers = page->AddCard("Big screen trailers");
  AddToggle(trailers, "Trailers", "In big screen, a trailer plays behind the game you rest on.",
            "big screen trailer video", &FrontendPrefs::big_screen_trailers, true);
  AddSeconds(trailers, "Start after", "How long to rest on a game before its trailer starts.",
             "big screen trailer delay wait", &FrontendPrefs::big_screen_trailer_delay_ms, 3000, 10);
  AddSeconds(trailers, "Skip the opening", "Starts trailers this far in, past rating cards and logos. Trailers under 20 seconds always start at the beginning.",
             "big screen trailer skip intro rating", &FrontendPrefs::big_screen_trailer_skip_ms, 3000, 15);
  AddToggle(trailers, "HD trailers only", "Skips trailers below 720p instead of showing them blurry.",
            "big screen trailer quality low resolution", &FrontendPrefs::big_screen_trailer_hd_only, false);
  AddToggle(trailers, "Trailer sound", "Plays a trailer's sound, not only its picture.",
            "big screen trailer audio sound mute", &FrontendPrefs::big_screen_trailer_sound, false);
  auto* volume_row = new SettingRow("Trailer volume", "How loud a trailer plays when its sound is on.");
  trailer_volume_ = new QSpinBox(volume_row);
  trailer_volume_->setRange(10, 100);
  trailer_volume_->setSingleStep(10);
  trailer_volume_->setSuffix("%");
  trailer_volume_->setValue(50);
  trailer_volume_->setMinimumWidth(90);
  volume_row->AddControl(trailer_volume_);
  trailers->AddRow(volume_row);
  nav_->RegisterRow(volume_row, "big screen trailer volume loud");
  connect(trailer_volume_, &QSpinBox::valueChanged, this, &SettingsPanel::Refresh);
  AddPrefField({.row = volume_row,
                .changed = [this] { return trailer_volume_->value() != trailer_volume_saved_; },
                .is_default = [this] { return trailer_volume_->value() == 50; },
                .revert = [this] { trailer_volume_->setValue(trailer_volume_saved_); },
                .reset = [this] { trailer_volume_->setValue(50); },
                .mark_saved = [this] { trailer_volume_saved_ = trailer_volume_->value(); }});

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
  if (tile_preview_ != nullptr && tile_status_ != nullptr && tile_source_ != nullptr) {
    tile_preview_->SetShown(tile_status_->isChecked(), tile_source_->isChecked());
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

  // Moved onto the Sources page once the schema loads (BuildSchemaPages).
  sources_card_ = page->AddCard("In the sidebar");
  AddToggle(sources_card_, "Show game counts", "Show how many games each source has next to its name.",
            "sidebar source numbers", &FrontendPrefs::sidebar_source_counts, true);
  AddToggle(sources_card_, "Covers on source rows",
            "Show a few of each source's games next to its name. Off shows a colored dot instead.",
            "sidebar source covers colors icons", &FrontendPrefs::sidebar_source_covers, true);
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
  const std::vector<QString> full = OrderSources({order.begin(), order.end()});
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

}  // namespace mira_gui
