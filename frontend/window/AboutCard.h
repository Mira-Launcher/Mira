#pragma once

#include "../settings/SettingsCard.h"

class QLabel;

namespace mira_gui {

// About Mira, as an in-window card (cards::Show): the logo, name and version (with a button that
// copies the version details for a bug report), then where the code lives, where to report a
// problem, who made it, its license and the service's log.
class AboutCard : public SettingsCard {
  Q_OBJECT

 public:
  explicit AboutCard(QWidget* parent = nullptr);

 private:
  void ApplyLogo();

  QLabel* logo_ = nullptr;
};

}  // namespace mira_gui
