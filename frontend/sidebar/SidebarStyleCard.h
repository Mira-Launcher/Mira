#pragma once

#include <optional>

#include "SettingsCard.h"
#include "SidebarGames.h"

class QAbstractButton;
class QSpinBox;

namespace mira_gui {

// How PINNED and RECENTLY PLAYED look: a row of tiles per section, each
// previewing the user's own games, then whether to show when each was last
// played and how many recently played games to list. Makes the rows; the
// caller puts them in a card, which takes ownership.
class SidebarStyleChoices : public QObject {
  Q_OBJECT

public:
  struct Choices {
    sidebar::Style pinned = sidebar::Style::Covers;
    sidebar::Style recent = sidebar::Style::Covers;
    int recent_count = 0;
    bool recent_when = true;  // "Yesterday" beside each recently played game

    bool operator==(const Choices&) const = default;
  };

  // `pinned` and `recent` are what the sections would list (`recent` as for the
  // count's maximum), so each tile previews the user's own games.
  SidebarStyleChoices(const Choices& choices, std::vector<GameSummary> pinned, std::vector<GameSummary> recent,
                      ArtworkStore* artwork, QObject* parent = nullptr);

  const Choices& Current() const { return choices_; }
  void SetChoices(const Choices& choices);
  QList<SettingRow*> Rows() const;

signals:
  void Changed(const mira_gui::SidebarStyleChoices::Choices& choices);

private:
  SettingRow* MakeStyleRow(const QString& label, bool recent);
  std::vector<sidebar::PreviewGame> PreviewGames(bool recent, sidebar::Style style) const;
  void Edited();
  void Sync();  // the controls from choices_

  Choices choices_;
  std::vector<GameSummary> pinned_;
  std::vector<GameSummary> recent_;
  ArtworkStore* artwork_;
  SettingRow* pinned_row_ = nullptr;
  SettingRow* recent_row_ = nullptr;
  SettingRow* when_row_ = nullptr;
  SettingRow* count_row_ = nullptr;
  std::vector<QAbstractButton*> pinned_tiles_;  // in StyleOptions() order
  std::vector<QAbstractButton*> recent_tiles_;
  Switch* when_ = nullptr;
  QSpinBox* recent_count_ = nullptr;
};

// The in-window card for the same choices, opened from the sidebar. Every
// choice applies at once (the sidebar beside it shows the result), so there is
// no Save; the window stores each one as it's made.
class SidebarStyleCard : public SettingsCard {
  Q_OBJECT

public:
  using Choices = SidebarStyleChoices::Choices;
  SidebarStyleCard(const Choices& choices, std::vector<GameSummary> pinned, std::vector<GameSummary> recent,
                   ArtworkStore* artwork, QWidget* parent = nullptr);

signals:
  void Changed(const mira_gui::SidebarStyleChoices::Choices& choices);
  void CloseRequested();
};

}  // namespace mira_gui
