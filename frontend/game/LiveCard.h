#pragma once

#include <QString>
#include <functional>
#include <vector>

#include "../components/SheetCard.h"

namespace mira_gui {

// A card whose rows fill in as mirad answers: what's shown before that is filled again after.
class LiveCard : public SheetCard {
  Q_OBJECT

 protected:
  using SheetCard::SheetCard;

  // Runs `fill` now and on every Refresh.
  void AddLive(std::function<void()> fill);
  // A row with a label and muted text on the right, one line cut short when it doesn't fit, and
  // the label's `tooltip`.
  void AddInfoRow(const QString& label, std::function<QString()> text,
                  std::function<QString()> tooltip = nullptr);
  void Refresh();

 private:
  std::vector<std::function<void()>> live_;
};

}  // namespace mira_gui
