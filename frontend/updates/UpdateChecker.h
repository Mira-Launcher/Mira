#pragma once

#include <QObject>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// What GET /v1/updates last said, shared by everything showing it. Asks again when something may
// have been installed or updates.check changes.
class UpdateChecker : public QObject {
  Q_OBJECT

 public:
  explicit UpdateChecker(QObject* parent = nullptr);

  const std::vector<UpdateInfo>& Updates() const { return updates_; }

  // While updates.check is on. `refresh` is someone asking: it checks regardless, past mirad's
  // cached answers.
  void Check(bool refresh = false);

 signals:
  void Changed();

 private:
  void Ask(bool refresh);

  std::vector<UpdateInfo> updates_;
  int request_ = 0;  // only the newest answer is kept
};

}  // namespace mira_gui
