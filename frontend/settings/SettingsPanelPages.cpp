// SettingsPanel's own pages, the ones not built from mirad's schema:
// Interface, Sidebar and Shortcuts, and the pref fields they register.
#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <algorithm>
#include <optional>
#include <type_traits>
#include <utility>

#include "../app/Display.h"
#include "../app/KeyBindings.h"
#include "../bigscreen/Choices.h"
#include "../sources/Sources.h"
#include "../theme/Theme.h"
#include "../widgets/ShortcutEdit.h"
#include "AppearancePreviews.h"
#include "SettingsCard.h"
#include "SettingsNav.h"
#include "SettingsPanel.h"

namespace mira_gui {
namespace {

// The card each shortcut sits in on the Shortcuts page.
QString ShortcutGroup(const QString& id) {
  static const QHash<QString, QString> kGroups = {
      {"focus_search", "Library"},
      {"clear_or_deselect", "Library"},
      {"toggle_hidden", "Library"},
      {"refresh", "Library"},
      {"zoom_in", "Tiles"},
      {"zoom_out", "Tiles"},
      {"reset_zoom", "Tiles"},
      {"play_stop", "Selected game"},
      {"details_settings", "Selected game"},
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
  const auto from_source =
      std::ranges::find_if(games.begin() + 1, games.end(), [](const GameSummary& game) {
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

Switch* SettingsPanel::AddToggle(SettingsCard* card, const QString& label, const QString& doc,
                                 const QString& search, std::optional<bool> FrontendPrefs::* member,
                                 bool fallback) {
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
  AddPrefField(
      {.row = row,
       .changed = [this,
                   index] { return toggles_[index].toggle->isChecked() != toggles_[index].saved; },
       .is_default =
           [this, index] {
             return toggles_[index].toggle->isChecked() == toggles_[index].fallback;
           },
       .revert = [this, index] { toggles_[index].toggle->setChecked(toggles_[index].saved); },
       .reset = [this, index] { toggles_[index].toggle->setChecked(toggles_[index].fallback); },
       .mark_saved = [this,
                      index] { toggles_[index].saved = toggles_[index].toggle->isChecked(); }});
  return toggle;
}

QDoubleSpinBox* SettingsPanel::AddSeconds(SettingsCard* card, const QString& label,
                                          const QString& doc, const QString& search,
                                          std::optional<int> FrontendPrefs::* member,
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
  AddPrefField(
      {.row = row,
       .changed =
           [this, index] {
             return qRound(seconds_[index].spin->value() * 1000) != seconds_[index].saved;
           },
       .is_default =
           [this, index] {
             return qRound(seconds_[index].spin->value() * 1000) == seconds_[index].fallback;
           },
       .revert = [this, index] { seconds_[index].spin->setValue(seconds_[index].saved / 1000.0); },
       .reset = [this,
                 index] { seconds_[index].spin->setValue(seconds_[index].fallback / 1000.0); },
       .mark_saved =
           [this, index] {
             seconds_[index].saved = qRound(seconds_[index].spin->value() * 1000);
           }});
  return spin;
}

// Both take a restart: Qt reads them before it opens a window.
void SettingsPanel::AddDisplayRows(SettingsCard* card) {
  const auto add_combo = [this, card](const QString& label, const QString& doc,
                                      const QString& search,
                                      const std::vector<std::pair<QString, QVariant>>& choices,
                                      const QVariant& current, const QVariant& fallback,
                                      std::function<void(const QVariant&)> store) {
    auto* row = new SettingRow(label, doc);
    auto* combo = new QComboBox(row);
    for (const auto& [text, value] : choices) combo->addItem(text, value);
    if (combo->findData(current) < 0) combo->addItem(current.toString(), current);  // hand-edited
    combo->setCurrentIndex(combo->findData(current));
    row->AddControl(combo);
    card->AddRow(row);
    nav_->RegisterRow(row, label + ' ' + search);
    connect(combo, &QComboBox::currentIndexChanged, this, &SettingsPanel::Refresh);
    auto saved = std::make_shared<QVariant>(combo->currentData());
    AddPrefField({.row = row,
                  .changed = [combo, saved] { return combo->currentData() != *saved; },
                  .is_default = [combo, fallback] { return combo->currentData() == fallback; },
                  .revert = [combo, saved] { combo->setCurrentIndex(combo->findData(*saved)); },
                  .reset = [combo, fallback] { combo->setCurrentIndex(combo->findData(fallback)); },
                  .mark_saved =
                      [this, combo, saved, store] {
                        if (combo->currentData() == *saved) return;
                        *saved = combo->currentData();
                        store(*saved);
                        restart_prompt_ = true;
                      }});
    return row;
  };

  std::vector<std::pair<QString, QVariant>> sizes;
  for (const double scale : {0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0}) {
    sizes.emplace_back(
        scale == 1.0 ? QString("100% (desktop's size)") : QString("%1%").arg(qRound(scale * 100)),
        scale);
  }
  add_combo("Interface size",
            "Makes everything in Mira larger or smaller than the desktop's own scale.",
            "scale zoom dpi gui ui size 4k tv", sizes, display::Scale(), 1.0,
            [](const QVariant& value) { display::SetScale(value.toDouble()); });

  SettingRow* server = add_combo(
      "Display server",
      "Wayland keeps Mira sharp on every screen when they use different scales. Big screen "
      "always uses X11.",
      "wayland x11 xwayland xcb blurry sharp platform",
      {{QString("Same as desktop (%1)")
            .arg(display::SystemServer() == "wayland" ? "Wayland" : "X11"),
        "desktop"},
       {"Wayland", "wayland"},
       {"X11", "x11"}},
      display::Server(), "desktop",
      [](const QVariant& value) { display::SetServer(value.toString()); });
  // Wayland needs a Wayland session.
  server->setEnabled(display::SystemServer() == "wayland" || display::Server() != "desktop");
  if (const QString hint = display::StretchHint(); !hint.isEmpty()) {
    auto* label = new QLabel(hint, server);
    label->setWordWrap(true);
    theme::SetStyleProperty(label, "role", "error");
    server->SetBelow(label);
  }
}

void SettingsPanel::BuildInterfacePage() {
  SettingsPage* page =
      nav_->AddCategory("Interface", CategoryGlyph("Interface"), CategoryNavGroup("Interface"));
  pages_["Interface"] = page;
  const std::vector<GameSummary> games = PreviewGames(previews_);

  SettingsCard* appearance = page->AddCard("Appearance");
  auto* theme_row =
      new SettingRow("Theme",
                     "The built-in themes take their colors from the desktop's color scheme. "
                     "Themes are .toml files in ~/.config/mira/themes. Edit mira-dark.toml "
                     "or mira-light.toml there to change the built-in themes, or copy one to "
                     "another name to add a theme of your own.");
  auto* tiles = new QWidget(theme_row);
  auto* tiles_layout = new QHBoxLayout(tiles);
  tiles_layout->setContentsMargins(0, 0, 0, 4);
  tiles_layout->setSpacing(10);
  themes_ = new QButtonGroup(this);
  QStringList names{"auto"};
  names << theme::Available();
  for (const QString& name : names) {
    auto* choice = new ThemeChoice(name, name == "auto" ? QString("Follow the desktop") : name,
                                   games, previews_.artwork, tiles);
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

  AddDisplayRows(appearance);

  SettingsCard* general = page->AddCard("General");
  AddToggle(general, "Start Mira when you log in",
            "Opens Mira in the tray when you log in, or in big screen if that's set to open at "
            "start.",
            "autostart login boot startup", &FrontendPrefs::start_on_login, false);
  AddToggle(general, "Scan the library on startup",
            "Scan the library each time Mira opens. The daemon already watches your folders while "
            "it runs, so "
            "this only catches changes made while it was stopped.",
            "scan startup", &FrontendPrefs::scan_on_startup, true);
  AddToggle(general, "Drag to select games",
            "Drag across the library to select several games at once.", "rubber band multiple",
            &FrontendPrefs::drag_select, true);
  AddToggle(general, "Double-click a game to play it",
            "Double-clicking a game in the library or on a source page starts it, or stops it "
            "while it runs.",
            "double click launch start play", &FrontendPrefs::double_click_play, true);
  AddToggle(general, "Show commands instead of asking for your password",
            "When Mira needs to install or remove a system package, it shows the command to run in "
            "a terminal "
            "instead of the system's password prompt.",
            "sudo pkexec terminal package install root polkit", &FrontendPrefs::copy_commands,
            false);

  SettingsCard* library = page->AddCard("Library");
  tile_preview_ = new TilePreview(TilePreviewGames(games), previews_.artwork);
  auto* preview_holder = new QWidget();
  auto* preview_layout = new QHBoxLayout(preview_holder);
  preview_layout->setContentsMargins(18, 4, 18, 10);
  preview_layout->addWidget(tile_preview_);
  preview_layout->addStretch(1);
  library->AddRow(preview_holder);
  AddToggle(library, "Filter tabs",
            "Tabs above the grid for All, Games, Apps, Installed and the other filters.",
            "library chips", &FrontendPrefs::library_filter_tabs, true);
  continue_row_ =
      AddToggle(library, "Continue playing",
                "Large cards for running and recently played games above the grid.",
                "library cards recently played recent", &FrontendPrefs::library_continue_row, true);
  continue_count_row_ =
      new SettingRow("Cards to show", "How many cards the Continue playing row shows.");
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
  AddToggle(library, "Apps in library",
            "Show apps in the library. Off, they're only on their source's page and in a search.",
            "library apps hide search sources", &FrontendPrefs::library_apps, true);
  AddToggle(library, "Apps under All", "List apps under the All tab too, not only under Apps.",
            "library apps all tab filter", &FrontendPrefs::library_apps_in_all, true);
  AddToggle(library, "Apps in Continue playing",
            "Show recently used apps in the Continue playing row, not only games.",
            "library cards recently played recent apps", &FrontendPrefs::library_continue_apps,
            false);
  tile_status_ = AddToggle(library, "Status on tiles",
                           "Show Needs install, Broken, Playing and the like on a tile.",
                           "tile badge", &FrontendPrefs::tile_status, true);
  tile_source_ = AddToggle(library, "Source on tiles",
                           "Name the store or launcher a game came from under its title.",
                           "tile store launcher source", &FrontendPrefs::tile_source_mark, true);
  AddToggle(library, "Same tile size everywhere",
            "One tile size for the library and every source page. Off, each page keeps its own.",
            "tile size zoom synced source pages", &FrontendPrefs::tile_size_synced, false);

  SettingsCard* layout = page->AddCard("Layout");
  layout_preview_ = new LayoutPreview(games, previews_.artwork);
  auto* layout_holder = new QWidget();
  auto* layout_holder_layout = new QHBoxLayout(layout_holder);
  layout_holder_layout->setContentsMargins(18, 4, 18, 10);
  layout_holder_layout->addWidget(layout_preview_);
  layout->AddRow(layout_holder);
  std::vector<size_t> layout_fields;
  const auto shape_row = [&](ShapeField& field, const QString& label, const QString& doc,
                             int maximum, std::optional<int> FrontendPrefs::* member) {
    field.member = member;
    auto* row = new SettingRow(label, doc);
    row->AddControl(MakeSlider(field, maximum));
    layout->AddRow(row);
    nav_->RegisterRow(row, label + " layout corner radius spacing");
    ShapeField* shape = &field;
    layout_fields.push_back(
        AddPrefField({.row = row,
                      .changed = [shape] { return shape->current != shape->saved; },
                      .is_default = [shape] { return !shape->current.has_value(); },
                      .revert =
                          [this, shape] {
                            shape->current = shape->saved;
                            RefreshShape(*shape);
                          },
                      .reset =
                          [this, shape] {
                            shape->current.reset();
                            RefreshShape(*shape);
                          },
                      .mark_saved = [shape] { shape->saved = shape->current; }}));
  };
  shape_row(tile_spacing_, "Tile gap",
            "Empty space around each tile. The zoom slider in the top bar changes the tile size.",
            40, &FrontendPrefs::tile_spacing);
  shape_row(grid_margin_, "Grid padding",
            "Space between the grid and the window edges and side panels.", 60,
            &FrontendPrefs::grid_margin);
  shape_row(tile_radius_, "Cover rounding", "Corner radius of game covers. 0 is square.", 40,
            &FrontendPrefs::tile_radius);
  shape_row(panel_radius_, "Panel rounding", "Corner radius of panels, cards and notices.", 24,
            &FrontendPrefs::panel_radius);
  shape_row(control_radius_, "Control rounding", "Corner radius of buttons, fields and dropdowns.",
            20, &FrontendPrefs::control_radius);
  AddCardReset(layout, std::move(layout_fields), {});
  RefreshShapeDefaults();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
          &SettingsPanel::RefreshShapeDefaults);
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
  field.value->setText(field.current ? QString("%1 px").arg(value)
                                     : QString("%1 px, theme default").arg(value));
  UpdatePreviews();
}

void SettingsPanel::RefreshShapeDefaults() {
  const theme::Tokens& defaults = theme::ThemeDefaults();
  tile_spacing_.theme_default = defaults.tile_spacing;
  grid_margin_.theme_default = defaults.grid_margin;
  tile_radius_.theme_default = defaults.radius_tile;
  panel_radius_.theme_default = defaults.radius_panel;
  control_radius_.theme_default = defaults.radius_control;
  for (ShapeField* field :
       {&tile_spacing_, &grid_margin_, &tile_radius_, &panel_radius_, &control_radius_}) {
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

template <typename T>
QComboBox* SettingsPanel::AddChoice(SettingsCard* card, const QString& label, const QString& doc,
                                    const QString& search, std::optional<T> FrontendPrefs::* member,
                                    const std::vector<std::pair<T, QString>>& options,
                                    const T& fallback) {
  const auto to_data = [](const T& value) {
    if constexpr (std::is_same_v<T, std::string>) {
      return QVariant(QString::fromStdString(value));
    } else {
      return QVariant(value);
    }
  };
  auto* row = new SettingRow(label, doc);
  auto* combo = new QComboBox(row);
  for (const auto& [value, text] : options) combo->addItem(text, to_data(value));
  combo->setCurrentIndex(combo->findData(to_data(fallback)));
  row->AddControl(combo);
  card->AddRow(row);
  nav_->RegisterRow(row, label + ' ' + search);
  connect(combo, &QComboBox::currentIndexChanged, this, &SettingsPanel::Refresh);
  const QVariant fallback_data = to_data(fallback);
  choices_.push_back(
      {.combo = combo,
       .load =
           [combo, member, fallback, to_data](const FrontendPrefs& prefs) {
             const int at = combo->findData(to_data((prefs.*member).value_or(fallback)));
             combo->setCurrentIndex(at >= 0 ? at : combo->findData(to_data(fallback)));
           },
       .store =
           [combo, member](FrontendPrefs& prefs) {
             if constexpr (std::is_same_v<T, std::string>) {
               prefs.*member = combo->currentData().toString().toStdString();
             } else {
               prefs.*member = combo->currentData().template value<T>();
             }
           },
       .saved = fallback_data});
  const size_t index = choices_.size() - 1;
  AddPrefField(
      {.row = row,
       .changed = [this,
                   index] { return choices_[index].combo->currentData() != choices_[index].saved; },
       .is_default = [combo, fallback_data] { return combo->currentData() == fallback_data; },
       .revert =
           [this, index] {
             choices_[index].combo->setCurrentIndex(
                 choices_[index].combo->findData(choices_[index].saved));
           },
       .reset = [combo, fallback_data] { combo->setCurrentIndex(combo->findData(fallback_data)); },
       .mark_saved = [this,
                      index] { choices_[index].saved = choices_[index].combo->currentData(); }});
  return combo;
}

void SettingsPanel::BuildBigScreenPage() {
  SettingsPage* page =
      nav_->AddCategory("Big screen", CategoryGlyph("Big screen"), CategoryNavGroup("Big screen"));
  pages_["Big screen"] = page;

  SettingsCard* starting = page->AddCard("Starting");
  AddToggle(starting, "Open in big screen when Mira starts",
            "Mira opens straight into big screen, also when it starts at login (Interface › "
            "General). The same as starting it with mira-gui --big-screen.",
            "big screen start launch login boot tv couch", &FrontendPrefs::big_screen_at_start,
            false);

  SettingsCard* screen = page->AddCard("Screen and sound");
  AddToggle(screen, "Large text", "Scales big screen up by 15%, for a TV across the room.",
            "big screen text size font tv", &FrontendPrefs::big_screen_large_text, false);
  AddToggle(screen, "Sounds", "Short sounds as you move around and pick things.",
            "big screen sound navigation audio", &FrontendPrefs::big_screen_sounds, true);
  AddToggle(screen, "Full screen games",
            "A game that opens in a window fills the screen, without borders.",
            "big screen fullscreen window borderless", &FrontendPrefs::big_screen_fullscreen_games,
            true);
  AddToggle(screen, "Show applications",
            "Lists apps alongside games. Apps tagged media always show, in their own row.",
            "big screen apps media", &FrontendPrefs::big_screen_show_apps, false);
  AddChoice(screen, "Sleep when idle",
            "Suspends the computer after this long on big screen with nothing playing or "
            "installing.",
            "big screen suspend idle sleep power", &FrontendPrefs::big_screen_idle_suspend,
            bigscreen::kIdle, 0);

  SettingsCard* controller = page->AddCard("Controller");
  AddChoice(controller, "Button labels",
            "Which names the button hints use. Match controller reads them from the controller.",
            "big screen controller gamepad xbox playstation nintendo",
            &FrontendPrefs::big_screen_buttons, bigscreen::kButtonKinds, std::string("auto"));
  AddChoice(controller, "Select with",
            "Which face button selects; the other goes back. B selects on Nintendo controllers.",
            "big screen controller confirm swap a b", &FrontendPrefs::big_screen_swap_confirm,
            std::vector<std::pair<bool, QString>>{{false, "Bottom button (A)"},
                                                  {true, "Right button (B)"}},
            false);
  AddChoice(controller, "Stick sensitivity",
            "How far the stick moves before it counts. High suits worn sticks less well.",
            "big screen controller stick deadzone", &FrontendPrefs::big_screen_stick,
            bigscreen::kStick, std::string("medium"));
  AddChoice(controller, "Scroll speed", "How fast lists move while a direction is held.",
            "big screen controller repeat scroll", &FrontendPrefs::big_screen_repeat,
            bigscreen::kRepeat, std::string("normal"));
  AddChoice(controller, "Vibration",
            "The controller rumbles when you pick something and knocks at the end of a list.",
            "big screen controller rumble vibration", &FrontendPrefs::big_screen_rumble,
            bigscreen::kRumble, std::string("light"));

  SettingsCard* trailers = page->AddCard("Trailers");
  AddToggle(trailers, "Trailers", "A trailer plays behind the game you rest on.",
            "big screen trailer video", &FrontendPrefs::big_screen_trailers, true);
  AddSeconds(trailers, "Start after", "How long to rest on a game before its trailer starts.",
             "big screen trailer delay wait", &FrontendPrefs::big_screen_trailer_delay_ms, 3000,
             10);
  AddSeconds(trailers, "Skip the opening",
             "Starts trailers this far in, past rating cards and logos. Trailers under 20 seconds "
             "always start at the beginning.",
             "big screen trailer skip intro rating", &FrontendPrefs::big_screen_trailer_skip_ms,
             3000, 15);
  AddToggle(trailers, "HD trailers only",
            "Skips trailers below 720p instead of showing them blurry.",
            "big screen trailer quality low resolution", &FrontendPrefs::big_screen_trailer_hd_only,
            false);
  AddToggle(trailers, "Trailer sound", "Plays a trailer's sound, not only its picture.",
            "big screen trailer audio sound mute", &FrontendPrefs::big_screen_trailer_sound, false);
  auto* volume_row =
      new SettingRow("Trailer volume", "How loud a trailer plays when its sound is on.");
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
}

void SettingsPanel::BuildSidebarPage() {
  SettingsPage* page =
      nav_->AddCategory("Sidebar", CategoryGlyph("Sidebar"), CategoryNavGroup("Sidebar"));
  pages_["Sidebar"] = page;

  SettingsCard* style = page->AddCard("Pinned and recently played");
  sidebar_style_ =
      new SidebarStyleChoices({}, previews_.pinned, previews_.recent, previews_.artwork, this);
  const QStringList searches = {
      "sidebar pinned style look covers hero banners shelf",
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
    field.changed = [this, member] {
      return sidebar_style_->Current().*member != sidebar_style_saved_.*member;
    };
    field.is_default = [this, member] {
      return sidebar_style_->Current().*member == Choices{}.*member;
    };
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
    field.mark_saved = [this, member] {
      sidebar_style_saved_.*member = sidebar_style_->Current().*member;
    };
    AddPrefField(std::move(field));
  };
  choice_field(rows[0], &Choices::pinned);
  choice_field(rows[1], &Choices::recent);
  choice_field(rows[2], &Choices::recent_when);
  choice_field(rows[3], &Choices::recent_count);

  // Moved onto the Sources page once the schema loads (BuildSchemaPages).
  sources_card_ = page->AddCard("In the sidebar");
  AddToggle(sources_card_, "Show game counts",
            "Show how many games each source has next to its name.", "sidebar source numbers",
            &FrontendPrefs::sidebar_source_counts, true);
  AddToggle(sources_card_, "Covers on source rows",
            "Show a few of each source's games next to its name. Off shows a colored dot instead.",
            "sidebar source covers colors icons", &FrontendPrefs::sidebar_source_covers, true);
  auto* sources_row =
      new SettingRow("Sources", "Which sources show, their order, and adding more.");
  auto* open = new QPushButton("Open Sources", sources_row);
  connect(open, &QPushButton::clicked, this, &SettingsPanel::SourcesPageRequested);
  sources_row->AddControl(open);
  sources_card_->AddRow(sources_row);
  nav_->RegisterRow(sources_row, "sidebar sources open");
}

void SettingsPanel::BuildShortcutsPage() {
  // The owning window registered every shortcut and loaded its overrides
  // before this screen could open, so the registry already holds current state.
  SettingsPage* page =
      nav_->AddCategory("Shortcuts", CategoryGlyph("Shortcuts"), CategoryNavGroup("Shortcuts"));
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
      for (const QKeySequence& alias : binding.extra_aliases)
        aliases << alias.toString(QKeySequence::NativeText);
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
    in_card->second.push_back(AddPrefField(
        {.row = row,
         .changed = [this,
                     index] { return shortcuts_[index].edit->Keys() != shortcuts_[index].saved; },
         .is_default =
             [this, index] {
               return shortcuts_[index].edit->Keys() == shortcuts_[index].default_keys;
             },
         .revert = [this, index] { shortcuts_[index].edit->SetKeys(shortcuts_[index].saved); },
         .reset = [this,
                   index] { shortcuts_[index].edit->SetKeys(shortcuts_[index].default_keys); },
         .mark_saved = [this,
                        index] { shortcuts_[index].saved = shortcuts_[index].edit->Keys(); }}));
  }
  for (auto& [card, fields] : cards) AddCardReset(card, std::move(fields), {});
}

}  // namespace mira_gui
