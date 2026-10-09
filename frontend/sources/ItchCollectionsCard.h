#pragma once

#include <vector>

#include "../settings/SettingsCard.h"

class QLabel;
class QLineEdit;
class QPushButton;

namespace mira_gui {

struct ItchCollectionsResult;

// The itch.io collections whose games show on the itch page: the account's
// own, plus any added by link. Added ones can be removed again.
class ItchCollectionsCard : public SettingsCard {
  Q_OBJECT

 public:
  explicit ItchCollectionsCard(QWidget* parent = nullptr);

  void Refresh();

 signals:
  // A collection was added or removed, so the page relists.
  void Changed();

 private:
  void ShowCollections(const ItchCollectionsResult& result);
  void Add();
  void SetStatus(const QString& text, bool error = false);

  std::vector<QWidget*> collection_rows_;  // between the intro and the status line
  QLabel* status_ = nullptr;
  QLineEdit* link_ = nullptr;
  QPushButton* add_ = nullptr;
};

}  // namespace mira_gui
