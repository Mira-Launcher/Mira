#pragma once

#include <QHash>
#include <QKeySequence>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "AppearancePreviews.h"
#include "SettingEditor.h"
#include "../sidebar/SidebarStyleCard.h"

class QButtonGroup;
class QLabel;
class QSlider;
class QDoubleSpinBox;
class QSpinBox;

namespace mira_gui {

class ArtworkStore;
class ChangeBar;
class SettingRow;
class SettingsCard;
class SettingsNavWidget;
class SettingsPage;
class ShortcutEdit;
class Switch;
class ThemeChoice;
class TilePreview;

// The settings screen's content, embedded in the library window in place of
// its body. Loads the schema and builds a page of cards per category, plus the
// frontend-only Interface, Sidebar and Shortcuts pages. Nothing applies until
// Save: changes are counted in a bar at the bottom that saves or discards
// them, and each changed row is marked.
class SettingsPanel : public QWidget {
  Q_OBJECT

public:
  // What the previews draw: the user's own games and their art.
  struct Previews {
    ArtworkStore* artwork = nullptr;
    std::vector<GameSummary> pinned;
    std::vector<GameSummary> recent;  // most recent first, running ones included
  };

  explicit SettingsPanel(Previews previews, QWidget* parent = nullptr);

  // FocusKey target for the frontend-only Sidebar page.
  static constexpr const char* kSidebarKey = "frontend.sidebar";
  // The Sources page's card of which sources the sidebar shows.
  static constexpr const char* kSidebarSourcesKey = "frontend.sidebar_sources";

  // Saves every change. Emits SaveFinished either way.
  void Save();

  // Switches to `key`'s page and focuses its field. No-op for an unknown key;
  // retried once the schema loads if called too early.
  void FocusKey(const QString& key);

  // Above the nav, e.g. the window's back button and title.
  void SetHeader(QWidget* header);
  // The nav column's width, e.g. the sidebar's, so opening Settings doesn't move the divider.
  void SetNavWidth(int width);

  // A button row at the end of `category`'s card titled `card`, for a one-off
  // action that belongs next to those settings. Added once the schema loads.
  void AddSectionAction(const QString& category, const QString& card, const QString& label, const QString& doc,
                        const QString& button_text, std::function<void()> activated);

  // True if anything differs from what was loaded or last saved.
  bool IsDirty() const { return ChangeCount() > 0; }

  // Puts every field back to what was loaded or last saved. Nothing applies
  // before Save, so there is nothing else to undo.
  void DiscardChanges();

signals:
  // Settings and display prefs have both loaded, so the panel can be shown complete.
  void Ready();
  void LoadFailed(QString error);
  void SaveFinished(bool ok, QString error);
  // The frontend.toml values just saved; the theme and shortcuts are already applied.
  void PrefsSaved(const mira_gui::FrontendPrefs& prefs);
  // The Settings > Sidebar "Open Sources" button was pressed.
  void SourcesPageRequested();

private:
  // A frontend.toml control, however it is drawn: whether it differs from what
  // was saved and from its default, and how to put it back to either.
  struct PrefField {
    SettingRow* row = nullptr;
    std::function<bool()> changed;
    std::function<bool()> is_default;
    std::function<void()> revert;    // to the saved value
    std::function<void()> reset;     // to the default
    std::function<void()> mark_saved;
  };

  // A frontend.toml switch bound to its FrontendPrefs field.
  struct PrefToggle {
    Switch* toggle = nullptr;
    std::optional<bool> FrontendPrefs::*member = nullptr;
    bool fallback = false;  // when frontend.toml doesn't set it
    bool saved = false;
  };

  // A number of seconds kept in milliseconds.
  struct PrefSeconds {
    QDoubleSpinBox* spin = nullptr;
    std::optional<int> FrontendPrefs::*member = nullptr;  // milliseconds
    int fallback = 0;
    int saved = 0;
  };

  // One shape adjustment over the theme; unset follows the theme.
  struct ShapeField {
    QSlider* slider = nullptr;
    QLabel* value = nullptr;
    std::optional<int> FrontendPrefs::*member = nullptr;
    int theme_default = 0;  // what unset resolves to in the current theme
    std::optional<int> current;
    std::optional<int> saved;
  };

  struct ShortcutField {
    QString id;
    ShortcutEdit* edit = nullptr;
    QKeySequence saved;  // the override, or the default if none
    QKeySequence default_keys;
  };

  struct CardReset {
    SettingsCard* card = nullptr;
    std::vector<size_t> prefs;   // into pref_fields_
    std::vector<size_t> schema;  // into fields_
  };

  void Load();
  void LoadFrontendPrefs();
  void BuildInterfacePage();
  void BuildSidebarPage();
  void BuildShortcutsPage();
  void BuildSchemaPages();
  // A Sources-style category: one folding card per source, its on switch in the header.
  void BuildSourceCards(SettingsPage* page, const std::vector<size_t>& rows);
  SettingRow* AddSchemaRow(SettingsCard* card, size_t index);
  Switch* AddToggle(SettingsCard* card, const QString& label, const QString& doc, const QString& search,
                    std::optional<bool> FrontendPrefs::*member, bool fallback);
  // A number of seconds kept in milliseconds; `maximum` in seconds.
  QDoubleSpinBox* AddSeconds(SettingsCard* card, const QString& label, const QString& doc, const QString& search,
                             std::optional<int> FrontendPrefs::*member, int fallback_ms, double maximum);
  // Returns the field's index in pref_fields_.
  size_t AddPrefField(PrefField field);
  // A card's "Reset to defaults" over these pref_fields_ and fields_ indices.
  void AddCardReset(SettingsCard* card, std::vector<size_t> prefs, std::vector<size_t> schema);
  QWidget* MakeSlider(ShapeField& field, int maximum);
  void RefreshShape(ShapeField& field);
  void RefreshShapeDefaults();
  void UpdatePreviews();
  QString SelectedTheme() const;
  void SelectTheme(const QString& name);
  struct SectionAction {
    QString category;
    QString card;
    QString label;
    QString doc;
    QString button_text;
    std::function<void()> activated;
  };
  void AppendSectionAction(const SectionAction& action);
  void LoadGameModeStatus();
  void PopulateRunnerCombos(const RunnersResult& result);
  // Recounts the changes, marks the rows and shows or hides the bar.
  void Refresh();
  int ChangeCount() const;
  bool PrefsDirty() const;
  void FinishSave(bool ok, const QString& error);

  Previews previews_;
  SettingsNavWidget* nav_ = nullptr;
  ChangeBar* change_bar_ = nullptr;
  bool loaded_ = false;  // Refresh ignores the edits loading itself makes

  // Interface
  QButtonGroup* themes_ = nullptr;
  QString theme_saved_;
  std::vector<PrefToggle> toggles_;
  std::vector<PrefSeconds> seconds_;
  QSpinBox* continue_count_ = nullptr;
  int continue_count_saved_ = 3;
  QSpinBox* trailer_volume_ = nullptr;
  int trailer_volume_saved_ = 50;
  SettingRow* continue_count_row_ = nullptr;
  Switch* continue_row_ = nullptr;
  TilePreview* tile_preview_ = nullptr;
  Switch* tile_status_ = nullptr;
  Switch* tile_source_ = nullptr;
  LayoutPreview* layout_preview_ = nullptr;
  ShapeField tile_spacing_;
  ShapeField grid_margin_;
  ShapeField tile_radius_;
  ShapeField panel_radius_;
  ShapeField control_radius_;

  // Sidebar
  SidebarStyleChoices* sidebar_style_ = nullptr;
  SidebarStyleChoices::Choices sidebar_style_saved_;
  SettingsCard* sources_card_ = nullptr;

  std::vector<ShortcutField> shortcuts_;
  std::vector<PrefField> pref_fields_;
  std::vector<SettingEditor> fields_;
  std::vector<CardReset> card_resets_;
  QString pending_focus_key_;  // FocusKey called before the schema arrived
  std::vector<SectionAction> section_actions_;
  QHash<QString, SettingsPage*> pages_;
  bool rows_built_ = false;
  // Read-only "is Feral GameMode installed/running" on the Launching page; not a config key.
  QLabel* gamemode_status_ = nullptr;
  int saves_pending_ = 0;
  int loads_pending_ = 2;  // the settings and the display prefs; Ready at zero
  void LoadDone();
  QString save_error_;
};

}  // namespace mira_gui
