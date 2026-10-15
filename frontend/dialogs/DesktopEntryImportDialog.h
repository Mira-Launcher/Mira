#pragma once

#include <QDialog>

class QListWidget;
class QPushButton;

namespace mira_gui {

// GET/POST /v1/desktop-entries: pick already-installed .desktop entries
// (including Flatpak apps, via their X-Flatpak key) to add as games.
class DesktopEntryImportDialog : public QDialog {
  Q_OBJECT

 public:
  explicit DesktopEntryImportDialog(QWidget* parent = nullptr);

 private:
  void Load();
  void Import();
  void UpdateImportEnabled();

  QListWidget* list_ = nullptr;
  QPushButton* import_ = nullptr;
};

}  // namespace mira_gui
