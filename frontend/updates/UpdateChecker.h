#pragma once

#include <QObject>
#include <functional>
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
  // cached answers. `done` runs once an answer lands, or fails.
  void Check(bool refresh = false, std::function<void()> done = {});

 signals:
  void Changed();

 private:
  void Ask(bool refresh);
  void Answered();

  std::vector<UpdateInfo> updates_;
  int request_ = 0;  // only the newest answer is kept
  std::vector<std::function<void()>> waiting_;
};

}  // namespace mira_gui
