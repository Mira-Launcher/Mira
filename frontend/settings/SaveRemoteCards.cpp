#include "SaveRemoteCards.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <json.hpp>

#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/Jobs.h"
#include "../components/Button.h"
#include "../components/CopyField.h"
#include "../components/Labels.h"
#include "../components/Metrics.h"
#include "../components/ModalOverlay.h"
#include "../components/ProgressRail.h"
#include "../components/SettingRow.h"
#include "../game/SavesText.h"
#include "../system/PackageInstall.h"

namespace mira_gui {
namespace {

struct Provider {
  const char* id;
  const char* label;
};
constexpr Provider kProviders[] = {{"onedrive", "OneDrive"},
                                   {"dropbox", "Dropbox"},
                                   {"webdav", "NAS (WebDAV)"},
                                   {"sftp", "SSH"},
                                   {"s3", "S3"}};

QString ProviderLabel(const std::string& id) {
  for (const Provider& provider : kProviders)
    if (id == provider.id) return provider.label;
  return QString::fromStdString(id);
}

bool SignsIn(const std::string& provider) {
  return provider == "onedrive" || provider == "dropbox";
}

}  // namespace

ConnectRemoteCard::ConnectRemoteCard(const std::string& slot, QWidget* parent)
    : SheetCard(QString(), parent), slot_(slot) {
  setFixedWidth(620);
  SetHeading(slot == "mirror" ? "Where should saves be copied too?" : "Where should saves sync?");
  SetClose("Close", [this] { emit CloseRequested(); });

  auto* choices = new QWidget(this);
  auto* grid = new QGridLayout(choices);
  grid->setContentsMargins(kRowPaddingX, 8, kRowPaddingX, 4);
  grid->setSpacing(10);
  int at = 0;
  for (const Provider& provider : kProviders) {
    auto* button = new QPushButton(provider.label, choices);
    button->setObjectName("choice_card");
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setMinimumHeight(44);
    connect(button, &QPushButton::clicked, this, [this, id = provider.id] { Pick(id); });
    grid->addWidget(button, at / 3, at % 3);
    providers_[provider.id] = button;
    ++at;
  }
  AddRow(choices);

  form_ = new QWidget(this);
  new QVBoxLayout(form_);
  form_->layout()->setContentsMargins(0, 0, 0, 0);
  static_cast<QVBoxLayout*>(form_->layout())->setSpacing(0);
  AddRow(form_);

  progress_ = new QWidget(this);
  auto* lines = new QVBoxLayout(progress_);
  lines->setContentsMargins(kRowPaddingX, 8, kRowPaddingX, 8);
  working_ = MakeLabel(progress_, QString(), text::kSubtle);
  lines->addWidget(working_);
  lines->addWidget(new ProgressRail(progress_));
  progress_->hide();
  AddRow(progress_);

  error_ = ErrorLine();
  QHBoxLayout* buttons = ButtonRow();
  auto* cancel = new QPushButton("Cancel", this);
  connect(cancel, &QPushButton::clicked, this, &ConnectRemoteCard::CloseRequested);
  buttons->addWidget(cancel);
  connect_ = new Button("Connect", ButtonKind::Primary, this);
  connect_->setEnabled(false);
  connect_->setDefault(true);
  connect(connect_, &QPushButton::clicked, this, &ConnectRemoteCard::Submit);
  buttons->addWidget(connect_);

  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (!live || job_.empty() || type != "saves.remote_sign_in") return;
            const auto event = nlohmann::json::parse(data, nullptr, false);
            if (!event.is_object() || event.value("slot", std::string()) != slot_) return;
            const QUrl url(QString::fromStdString(event.value("url", std::string())));
            // Only rclone's own page on this computer.
            if (url.scheme() != "http" || url.host() != "127.0.0.1") return;
            SetWorking("Sign in in your browser…");
            QDesktopServices::openUrl(url);
          });
}

ConnectRemoteCard::~ConnectRemoteCard() {
  if (!job_.empty()) jobs::Cancel(EventHub::Instance(), job_, [](const ApiError&) {});
}

void ConnectRemoteCard::Pick(const std::string& provider) {
  if (provider == provider_) return;
  provider_ = provider;
  Fill();
}

QLineEdit* ConnectRemoteCard::Field(const std::string& key, const QString& label,
                                    const QString& placeholder, bool secret) {
  auto* row = new SettingRow(label, QString(), form_);
  auto* edit = new QLineEdit(row);
  edit->setMinimumWidth(320);
  edit->setPlaceholderText(placeholder);
  edit->setAccessibleName(label);
  if (secret) edit->setEchoMode(QLineEdit::Password);
  connect(edit, &QLineEdit::returnPressed, this, &ConnectRemoteCard::Submit);
  row->AddControl(edit, 1);
  form_->layout()->addWidget(row);
  fields_[key] = edit;
  return edit;
}

void ConnectRemoteCard::Fill() {
  error_->hide();
  key_file_.clear();
  host_keys_.clear();
  fields_.clear();
  for (QWidget* old : form_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
    old->hide();
    old->deleteLater();
  }
  connect_->setText(SignsIn(provider_) ? "Sign in…" : "Connect");
  connect_->setEnabled(true);
  Field("name", "Name", "As Mira shows it")->setText(ProviderLabel(provider_));
  if (provider_ == "webdav") {
    Field("url", "Address", "https://nas.example:8443/dav")->setFocus();
    Field("user", "User name");
    Field("password", "Password", {}, true);
  } else if (provider_ == "sftp") {
    Field("host", "Server", "nas.example")->setFocus();
    Field("port", "Port", "22");
    Field("user", "User name");
    Field("password", "Password", "Empty to use an SSH key", true);
    Field("key_file", "SSH key", "Empty for one Mira makes");
  } else if (provider_ == "s3") {
    Field("endpoint", "Address", "https://s3.example")->setFocus();
    Field("region", "Region", "Optional");
    Field("access_key_id", "Access key ID");
    Field("secret_access_key", "Secret access key", {}, true);
  }
  Field("folder", provider_ == "s3" ? "Bucket and folder" : "Folder",
        provider_ == "s3" ? "bucket/folder" : "Optional");
  if (provider_ == "webdav" || provider_ == "s3") {
    QLineEdit* file = Field("ca_file", "Certificate", "Only for a server that signs its own");
    auto* browse = new QPushButton("Choose…", form_);
    browse->ensurePolished();
    file->setMinimumWidth(320 - browse->sizeHint().width() - 6);  // as wide as the rest with it
    connect(browse, &QPushButton::clicked, this, [this, file] {
      const QString chosen = QFileDialog::getOpenFileName(this, "The server's certificate");
      if (!chosen.isEmpty()) file->setText(chosen);
    });
    static_cast<SettingRow*>(file->parentWidget())->AddControl(browse);
  }
}

std::string ConnectRemoteCard::Value(const std::string& key) const {
  const auto found = fields_.find(key);
  if (found == fields_.end()) return {};
  const QString text = found->second->text();
  return (found->second->echoMode() == QLineEdit::Password ? text : text.trimmed()).toStdString();
}

void ConnectRemoteCard::SetWorking(const QString& what) {
  const bool working = !what.isEmpty();
  working_->setText(what);
  progress_->setVisible(working);
  form_->setEnabled(!working);
  connect_->setEnabled(!working);
  for (const auto& [id, button] : providers_) button->setEnabled(!working);
  if (working) error_->hide();
}

void ConnectRemoteCard::Fail(const ApiError& error) {
  SetWorking({});
  error_->setText(error_help::Describe(error));
  error_->show();
}

void ConnectRemoteCard::Submit() {
  if (!connect_->isEnabled() || provider_.empty()) return;
  if (provider_ != "sftp") return Connect({});
  if (!host_keys_.empty()) return Connect(host_keys_);
  SetWorking("Getting ready…");
  system::EnsurePackages(this, "ssh", "Syncing over SSH", [this](bool ready) {
    if (!ready) return SetWorking({});
    const auto scan = [this] {
      SetWorking("Checking the server's key…");
      api::ScanHostKeysAsync(this, Value("host"), Value("port"), [this](api::HostKeys keys) {
        if (!keys.ok) return Fail(keys.error);
        AskTrust(keys);
      });
    };
    if (!Value("password").empty() || !Value("key_file").empty()) return scan();
    api::MakeSshKeyAsync(this, [this, scan](api::SshKey key) {
      if (!key.ok) return Fail(key.error);
      key_file_ = key.path;
      form_->layout()->addWidget(
          MakeLabel(form_, "Add this key to the server's authorized_keys:", text::kSubtle));
      form_->layout()->addWidget(new CopyField(QString::fromStdString(key.public_key), form_));
      scan();
    });
  });
}

void ConnectRemoteCard::AskTrust(const api::HostKeys& keys) {
  SetWorking({});
  host_keys_ = keys.keys;
  // Another server's key must be asked about again.
  for (const char* key : {"host", "port"})
    connect(fields_.at(key), &QLineEdit::textEdited, this, [this] {
      host_keys_.clear();
      connect_->setText("Connect");
    });
  QStringList prints;
  for (const std::string& print : keys.fingerprints) prints << QString::fromStdString(print);
  form_->layout()->addWidget(
      MakeLabel(form_, "Check that the server shows one of these keys:", text::kSubtle));
  form_->layout()->addWidget(new CopyField(prints.join('\n'), form_));
  connect_->setText("Trust and connect");
}

void ConnectRemoteCard::Connect(const std::string& host_keys) {
  api::SaveRemoteRequest request{.slot = slot_,
                                 .provider = provider_,
                                 .name = Value("name"),
                                 .folder = Value("folder"),
                                 .ca_file = Value("ca_file"),
                                 .host_keys = host_keys,
                                 .values = {}};
  for (const auto& [key, edit] : fields_) {
    if (key == "name" || key == "folder" || key == "ca_file") continue;
    if (std::string value = Value(key); !value.empty()) request.values[key] = std::move(value);
  }
  if (!key_file_.empty() && !request.values.contains("password") &&
      !request.values.contains("key_file"))
    request.values["key_file"] = key_file_;
  SetWorking(SignsIn(provider_) ? "Getting ready…" : "Connecting…");
  job_ = api::ConnectSaveRemoteAsync(this, request, [this](api::SaveRemoteState state) {
    job_.clear();
    if (!state.ok) return Fail(state.error);
    emit Connected();
  });
}

SaveRemoteCard::SaveRemoteCard(QWidget* parent) : Card("Save sync", parent) {
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string&, bool live) {
            if (live && (type == "saves.remote_changed" || type == "game.saves_changed")) Load();
          });
  Load();
}

void SaveRemoteCard::Load() {
  api::GetSaveRemoteAsync(this, [this](api::SaveRemoteState state) {
    if (state.ok) Show(state);
  });
}

void SaveRemoteCard::Show(const api::SaveRemoteState& state) {
  ClearRows();
  const QDateTime now = QDateTime::currentDateTime();
  const auto button = [](SettingRow* row, const QString& label, auto run) {
    auto* made = new QPushButton(label, row);
    QObject::connect(made, &QPushButton::clicked, row, run);
    row->AddControl(made);
    return made;
  };
  if (!state.primary) {
    auto* row = new SettingRow("Not connected",
                               "Keep every game's saves in step between computers through "
                               "storage of your own: OneDrive, Dropbox, a NAS, SSH or S3. Games "
                               "whose store keeps their saves are left to it.",
                               this);
    button(row, "Connect…", [this] { OpenConnect("primary"); });
    AddRow(row);
    emit Rearranged();
    return;
  }
  const QString name = QString::fromStdString(state.primary->name);
  auto* row = new SettingRow(name, "Where saves sync when a game starts and closes", this);
  QStringList about;
  if (const QString label = ProviderLabel(state.primary->provider); label != name) about << label;
  if (state.synced_at > 0) about << "Synced " + saves_text::When(state.synced_at, now, true);
  const QString detail = about.join(" · ");
  row->AddControl(MakeLabel(row, detail, text::kSubtle, /*wrap=*/false));
  QPushButton* sync = button(row, syncing_ ? "Syncing…" : "Sync all now", [this] {
    syncing_ = true;
    Load();
    api::SyncAllSavesAsync(this, [this](api::SaveRemoteState done) {
      syncing_ = false;
      Load();
      if (!done.ok) notify::FailedRequest(this, "Could not sync the saves", done.error);
    });
  });
  sync->setEnabled(!syncing_);
  button(row, "Disconnect", [this, name] { Disconnect("primary", name); });
  AddRow(row);

  auto* mirror = new SettingRow("Also copy to",
                                "A second place saves are copied to after each sync. It's only "
                                "written to, never synced from.",
                                this);
  if (state.mirror) {
    const QString to = QString::fromStdString(state.mirror->name);
    mirror->AddControl(MakeLabel(mirror, to, text::kSubtle, /*wrap=*/false));
    button(mirror, "Remove", [this, to] { Disconnect("mirror", to); });
  } else {
    button(mirror, "Choose…", [this] { OpenConnect("mirror"); });
  }
  AddRow(mirror);

  for (const api::PendingSave& game : state.pending) {
    auto* waiting = new SettingRow(QString::fromStdString(game.name), QString(), this);
    const QString why = game.state == "conflict"          ? "Changed here and on " + name
                        : game.slot == "mirror"           ? QString("Waiting to be copied")
                        : game.error == "remote_offline"  ? QString("Waiting: offline")
                        : game.error == "remote_sign_in"  ? QString("Waiting: sign in again")
                        : game.error == "remote_full"     ? QString("Waiting: no room left")
                        : game.error == "saves_too_large" ? QString("Too big to sync")
                                                          : QString("Waiting to upload");
    waiting->AddControl(MakeLabel(waiting, why, text::kSubtle, /*wrap=*/false));
    AddRow(waiting);
  }
  emit Rearranged();
}

void SaveRemoteCard::Disconnect(const std::string& slot, const QString& name) {
  if (!notify::Confirm(this, "Disconnect " + name + "?",
                       "Mira forgets how to sign in. Saves already there and the ones on this "
                       "computer stay as they are.",
                       "Disconnect"))
    return;
  api::DisconnectSaveRemoteAsync(this, slot, [this](api::SaveRemoteState state) {
    if (!state.ok) return notify::FailedRequest(this, "Could not disconnect", state.error);
    Show(state);
  });
}

void SaveRemoteCard::OpenConnect(const std::string& slot) {
  if (overlay_ != nullptr) return;
  QWidget* host = window();
  overlay_ = new ModalOverlay(host);
  auto* column = new QVBoxLayout(overlay_);
  column->setContentsMargins(32, 32, 32, 32);
  auto* card = new ConnectRemoteCard(slot, overlay_);
  column->addStretch(1);
  column->addWidget(card, 0, Qt::AlignHCenter);
  column->addStretch(1);
  // A click beside it doesn't close it: what's typed would be lost.
  const auto close = [this] {
    if (overlay_ == nullptr) return;
    overlay_->hide();
    overlay_->deleteLater();
    overlay_ = nullptr;
  };
  connect(card, &ConnectRemoteCard::CloseRequested, this, close);
  connect(card, &ConnectRemoteCard::Connected, this, [this, close] {
    close();
    Load();
  });
  // Follows the window, and goes with this card.
  class Follow : public QObject {
   public:
    explicit Follow(QWidget* overlay) : QObject(overlay), overlay_(overlay) {}
    bool eventFilter(QObject* watched, QEvent* event) override {
      if (event->type() == QEvent::Resize)
        overlay_->setGeometry(static_cast<QWidget*>(watched)->rect());
      return false;
    }
    QWidget* overlay_;
  };
  host->installEventFilter(new Follow(overlay_));
  connect(this, &QObject::destroyed, overlay_, &QObject::deleteLater);
  overlay_->setGeometry(host->rect());
  overlay_->show();
  overlay_->raise();
}

}  // namespace mira_gui
