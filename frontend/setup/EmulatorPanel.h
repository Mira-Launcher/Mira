#pragma once

#include <QWidget>

#include "../client/Types.h"

class QLabel;

namespace mira_gui {

// What an emulator needs before its games run: the emulator itself, downloaded, from the system's
// packages or a program you choose, then the BIOS, firmware or keys only you can supply.
class EmulatorPanel : public QWidget {
  Q_OBJECT

 public:
  EmulatorPanel(const EmulatorInfo& emulator, QWidget* parent);

  void ShowFailure(const QString& what, const ApiError& error);

 signals:
  // Installed, or firmware added: the page asks mirad again.
  void Changed();

 private:
  void Install();
  void ChooseProgram();
  void AddFirmware(const EmulatorFirmware& firmware);
  void SendFirmware(const std::string& firmware_id, const std::vector<std::string>& paths, bool replace);

  EmulatorInfo emulator_;
  QLabel* error_ = nullptr;
  QWidget* install_ = nullptr;
};

}  // namespace mira_gui
