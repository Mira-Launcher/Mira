#pragma once

#include <QPointer>
#include <functional>
#include <map>
#include <string>

#include "../client/api/SaveRemote.h"
#include "../components/SheetCard.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class ModalOverlay;
class ProgressRail;

// Connects a remote for saves: the provider, then what it needs, then (SSH) the server's key to
// trust.
class ConnectRemoteCard : public SheetCard {
  Q_OBJECT

 public:
  ConnectRemoteCard(const std::string& slot, QWidget* parent = nullptr);
  ~ConnectRemoteCard() override;

 signals:
  void Connected();
  void CloseRequested();

 private:
  void Pick(const std::string& provider);
  void Fill();
  QLineEdit* Field(const std::string& key, const QString& label, const QString& placeholder = {},
                   bool secret = false);
  std::string Value(const std::string& key) const;
  void Submit();
  // After the SSH server's key is known: asks whether to trust it.
  void AskTrust(const api::HostKeys& keys);
  void Connect(const std::string& host_keys);
  void SetWorking(const QString& what);
  void Fail(const ApiError& error);

  std::string slot_;
  std::string provider_;
  std::string key_file_;  // SSH: the key Mira made
  std::string host_keys_;
  std::string job_;  // the connect under way
  std::map<std::string, QPushButton*> providers_;
  std::map<std::string, QLineEdit*> fields_;
  QWidget* form_ = nullptr;
  QLabel* error_ = nullptr;
  QLabel* working_ = nullptr;
  QWidget* progress_ = nullptr;
  QPushButton* connect_ = nullptr;
};

// Settings' Save sync card: what's connected, its mirror, and the saves waiting.
class SaveRemoteCard : public Card {
  Q_OBJECT

 public:
  explicit SaveRemoteCard(QWidget* parent = nullptr);

 signals:
  // Rows were rebuilt, so the page's columns may need new heights.
  void Rearranged();

 private:
  void Load();
  void Show(const api::SaveRemoteState& state);
  void OpenConnect(const std::string& slot);
  void Disconnect(const std::string& slot, const QString& name);

  QPointer<ModalOverlay> overlay_;
  bool syncing_ = false;
};

}  // namespace mira_gui
