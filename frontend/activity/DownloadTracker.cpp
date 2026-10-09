#include "DownloadTracker.h"

#include <QLocale>
#include <QStringList>
#include <QTimer>

#include <json.hpp>

#include "../client/JsonMapping.h"
#include "../widgets/Labels.h"

#include <algorithm>

#include "../client/Jobs.h"
#include "../client/Events.h"
#include "../client/api/Games.h"
#include "../client/api/Stores.h"

namespace mira_gui {
namespace {

bool ToState(const std::string& state, DownloadTracker::State* out) {
  if (state == "started") {
    *out = DownloadTracker::State::Running;
  } else if (state == "finished") {
    *out = DownloadTracker::State::Finished;
  } else if (state == "failed") {
    *out = DownloadTracker::State::Failed;
  } else if (state == "paused") {
    *out = DownloadTracker::State::Paused;
  } else {
    return false;
  }
  return true;
}

// The helper a setup downloads.
QString ToolName(const QString& source) {
  if (source == "umu") return "umu-launcher";
  return source;
}

}  // namespace

DownloadTracker::DownloadTracker(QObject* parent) : QObject(parent) {}

QString DownloadTracker::KeyFor(Kind kind, const QString& source, const QString& ref) {
  switch (kind) {
    case Kind::Game: return "game:" + ref;
    case Kind::Title: return source + ":" + ref;
    case Kind::Launcher: return "launcher:" + source;
    case Kind::Tool: return "tool:" + source;
    case Kind::Runner: return "runner:" + source + "/" + ref;
    case Kind::Job: return "job:" + ref;
  }
  return QString();
}

QString DownloadTracker::LogChannelFor(const Entry& entry) {
  switch (entry.kind) {
    case Kind::Game: return "game:" + entry.ref;
    case Kind::Title: return entry.source == "steam" ? QString("daemon") : "install:" + entry.source + ":" + entry.ref;
    case Kind::Launcher:
    case Kind::Tool: return "setup:" + entry.source;
    case Kind::Runner: return "runner:" + entry.source + ":" + entry.ref;
    case Kind::Job: return "daemon";
  }
  return "daemon";
}

bool DownloadTracker::CanPause(const Entry& entry) {
  return entry.kind == Kind::Title && (entry.source == "epic" || entry.source == "gog" || entry.source == "amazon");
}

QString DownloadTracker::JobFor(const Entry& entry) const {
  if (entry.state != State::Running) return {};
  QString target;  // the target mirad started the job with
  switch (entry.kind) {
    case Kind::Title: target = entry.source + "-" + entry.ref; break;
    case Kind::Game: target = entry.ref; break;
    case Kind::Launcher:
    case Kind::Tool: target = entry.source; break;
    case Kind::Runner: target = entry.source + ":" + entry.ref; break;
    case Kind::Job: return {};
  }
  const auto found = job_by_target_.find(target);
  return found == job_by_target_.end() ? QString() : found->second;
}

QString DownloadTracker::ProgressText(const Entry& entry, bool short_form) {
  if (entry.progress < 0) return {};
  QStringList parts{QString("%1%").arg(qRound(entry.progress * 100))};
  if (!short_form && entry.bytes_per_second > 0) {
    parts << SizeText(static_cast<qint64>(entry.bytes_per_second)) + "/s";
  }
  if (entry.eta_seconds > 0) {
    const qint64 minutes = (entry.eta_seconds + 59) / 60;
    parts << (minutes >= 60 ? QString("%1 h %2 min left").arg(minutes / 60).arg(minutes % 60)
                            : QString("%1 min left").arg(minutes));
  }
  return parts.join(" · ");
}

DownloadTracker::TileProgress DownloadTracker::TileProgressFor(const Entry& entry) {
  TileProgress tile;
  tile.fraction = entry.progress;
  const QString doing = entry.source == "humble" ? "Downloading…" : entry.update ? "Updating…" : "Installing…";
  tile.status = entry.progress >= 0 ? ProgressText(entry, /*short_form=*/true) : doing;
  if (entry.bytes_per_second > 0) {
    tile.detail = SizeText(static_cast<qint64>(entry.bytes_per_second)) + "/s";
  } else if (entry.bytes > 0) {
    tile.detail = SizeText(entry.bytes) + " written";
  }
  return tile;
}

bool DownloadTracker::HandleJobEvent(const std::string& type, const std::string& data) {
  if (!type.starts_with("job.")) return false;
  const nlohmann::json event = nlohmann::json::parse(data, nullptr, false);
  if (!event.is_object()) return true;
  // Jobs whose work already has its own row: installs and updates (library.,
  // launcher. and game.install.*), runner downloads (runners.download.*) and
  // runner tool setups (umu./winetricks.setup.*). A store's own setup is only a job.
  const std::string kind = mapping::Str(event, "kind");
  const std::string target = mapping::Str(event, "target");
  const QString id = QString::fromStdString(mapping::Str(event, "id"));
  if (kind == "install" || kind == "update" || kind == "runner" ||
      (kind == "setup" && (target == "umu" || target == "winetricks"))) {
    // Kept so that row can cancel it.
    if (type == "job.started") {
      job_by_target_[QString::fromStdString(target)] = id;
      emit Changed(QString());  // its row can offer Cancel now
    } else if (type == "job.finished" || type == "job.failed") {
      std::erase_if(job_by_target_, [&id](const auto& entry) { return entry.second == id; });
    }
    return true;
  }
  const QString key = KeyFor(Kind::Job, QString(), id);
  // Progress for a job whose start this stream never saw has no name to show.
  if (type != "job.started" && Find(key) == nullptr) return true;
  Entry& entry = Upsert(Kind::Job, QString::fromStdString(mapping::Str(event, "kind")), id);
  if (type == "job.started") {
    entry.state = State::Running;
    NoteTitle("job", id, QString::fromStdString(mapping::Str(event, "label")));
  } else if (type == "job.progress") {
    const int total = mapping::Int(event, "total");
    entry.progress = total > 0 ? static_cast<double>(mapping::Int(event, "done")) / total : -1;
    entry.message = QString::fromStdString(mapping::Str(event, "message"));
  } else if (type == "job.finished") {
    // A job without steps (a scan, an import) is routine: its caller reports
    // the outcome, and a "Done" row per startup scan would only pile up.
    if (entry.progress < 0) {
      std::erase_if(entries_, [&key](const Entry& e) { return e.key == key; });
      emit Changed(key);
      return true;
    }
    entry.state = State::Finished;
  } else if (type == "job.failed") {
    entry.state = State::Failed;
    const nlohmann::json error = event.contains("error") ? event["error"] : nlohmann::json::object();
    entry.error = mapping::ToApiError(error);
  }
  emit Changed(entry.key);
  return true;
}

void DownloadTracker::LoadPaused() {
  api::ListPausedInstallsAsync(this, [this](PausedInstallsResult result) {
    if (!result.ok) return;
    for (const PausedInstall& install : result.installs) {
      const QString source = QString::fromStdString(install.source);
      const QString ref = QString::fromStdString(install.ref);
      Entry& entry = Upsert(Kind::Title, source, ref);
      if (entry.state != State::Running) {
        entry.state = State::Paused;
        entry.update = install.update;
      }
      if (NameFor(entry) == ref) ResolveNames(source);
    }
    emit Changed(QString());
  });
}

void DownloadTracker::RecheckJobs() {
  for (const Entry& entry : entries_) {
    if (entry.kind != Kind::Job || entry.state != State::Running) continue;
    const QString key = entry.key;
    jobs::Check(this, entry.ref.toStdString(), [this, key](jobs::Outcome outcome) {
      const auto found = std::find_if(entries_.begin(), entries_.end(), [&key](const Entry& e) { return e.key == key; });
      if (found == entries_.end() || found->state != State::Running) return;
      found->state = outcome.ok ? State::Finished : State::Failed;
      found->error = outcome.error;
      emit Changed(key);
    });
  }
}

bool DownloadTracker::HandleEvent(const std::string& type, const std::string& data) {
  if (HandleJobEvent(type, data)) return true;
  State state;
  if (InstallEvent install; events::ParseInstallEvent(type, data, &install)) {
    if (!ToState(install.state, &state)) return true;
    Entry& entry = Upsert(Kind::Game, QString(), QString::fromStdString(install.id));
    entry.state = state;
    entry.error = install.error;
    if (state == State::Running) entry.bytes = 0;
    if (DropIfCancelled(entry)) return true;
    emit Changed(entry.key);
    return true;
  }
  if (RunnerDownloadEvent runner; events::ParseRunnerDownload(type, data, &runner)) {
    const bool progress = runner.state == "progress";
    if (progress) {
      state = State::Running;
    } else if (!ToState(runner.state, &state)) {
      return true;
    }
    // Kron4ek's variants share a tag, so key by the release's name.
    const std::string& ref = runner.name.empty() ? runner.tag : runner.name;
    Entry& entry = Upsert(Kind::Runner, QString::fromStdString(runner.kind), QString::fromStdString(ref));
    if (!runner.label.empty()) NoteTitle("runner:" + entry.source, entry.ref, QString::fromStdString(runner.label));
    entry.state = state;
    entry.error = runner.error;
    entry.progress = progress ? runner.progress : -1;
    if (DropIfCancelled(entry)) return true;
    emit Changed(entry.key);
    return true;
  }
  StoreEvent store;
  if (!events::ParseStoreEvent(type, data, &store)) return false;
  if (store.state == "progress") {
    // A launcher's install reports how far along it is too (Office's own figure); it has no title.
    const bool launcher = store.kind == "setup" && type.starts_with("launcher.");
    Entry& entry = Upsert(launcher ? Kind::Launcher : Kind::Title, QString::fromStdString(store.source),
                          QString::fromStdString(store.ref));
    entry.state = State::Running;
    entry.progress = store.progress;
    entry.eta_seconds = store.eta_seconds;
    entry.bytes_per_second = store.bytes_per_second;
    emit Changed(entry.key);
    return true;
  }
  if (!ToState(store.state, &state)) return true;
  const QString source = QString::fromStdString(store.source);
  const QString ref = QString::fromStdString(store.ref);
  Kind kind = Kind::Title;
  if (store.kind == "setup") {
    kind = type.starts_with("launcher.") ? Kind::Launcher : Kind::Tool;
  }
  Entry& entry = Upsert(kind, source, ref);
  entry.state = state;
  entry.update = store.update;
  entry.error = store.error;
  if (state == State::Running) {
    entry.progress = -1;
    entry.eta_seconds = -1;
    entry.bytes_per_second = -1;
  }
  if (DropIfCancelled(entry)) return true;
  const QString key = entry.key;
  if (kind == Kind::Title && NameFor(entry) == ref) ResolveNames(source);
  emit Changed(key);
  return true;
}

bool DownloadTracker::DropIfCancelled(const Entry& entry) {
  if (entry.state != State::Failed || entry.error.code != "cancelled") return false;
  const QString key = entry.key;
  std::erase_if(entries_, [&key](const Entry& e) { return e.key == key; });
  emit Changed(key);
  return true;
}

DownloadTracker::Entry& DownloadTracker::Upsert(Kind kind, const QString& source, const QString& ref) {
  const QString key = KeyFor(kind, source, ref);
  auto found = std::find_if(entries_.begin(), entries_.end(), [&key](const Entry& e) { return e.key == key; });
  if (found == entries_.end()) {
    Entry entry;
    entry.key = key;
    entry.kind = kind;
    entry.source = source;
    entry.ref = ref;
    entries_.insert(entries_.begin(), std::move(entry));
    found = entries_.begin();
  } else if (found != entries_.begin()) {
    // Newest activity first.
    std::rotate(entries_.begin(), found, found + 1);
    found = entries_.begin();
  }
  found->changed = QDateTime::currentDateTime();

  // Only game installers can say how far along they are.
  if (kind == Kind::Game) {
    if (poll_ == nullptr) {
      poll_ = new QTimer(this);
      poll_->setInterval(2000);
      connect(poll_, &QTimer::timeout, this, &DownloadTracker::Poll);
    }
    poll_->start();
  }
  return *found;
}

const DownloadTracker::Entry* DownloadTracker::Find(const QString& key) const {
  for (const Entry& entry : entries_) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

int DownloadTracker::RunningCount() const {
  return static_cast<int>(
      std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.state == State::Running; }));
}

void DownloadTracker::ClearFinished() {
  std::erase_if(entries_, [](const Entry& e) { return e.state != State::Running && e.state != State::Paused; });
  emit Changed(QString());
}

QString DownloadTracker::NameFor(const Entry& entry) const {
  const auto game = [this](const QString& id) {
    return game_name ? game_name(id.toStdString()) : QString();
  };
  const auto source = [this](const QString& id) { return source_name ? source_name(id) : id; };
  switch (entry.kind) {
    case Kind::Game: {
      const QString name = game(entry.ref);
      return name.isEmpty() ? entry.ref : name;
    }
    case Kind::Title: {
      if (const QString title = titles_.value(entry.source + ":" + entry.ref); !title.isEmpty()) return title;
      const QString name = game(entry.source + "-" + entry.ref);
      return name.isEmpty() ? entry.ref : name;
    }
    case Kind::Launcher: return source(entry.source);
    case Kind::Tool: return ToolName(entry.source);
    case Kind::Runner: {
      const QString label = titles_.value("runner:" + entry.source + ":" + entry.ref);
      return label.isEmpty() ? entry.ref : label;
    }
    case Kind::Job: {
      const QString label = titles_.value("job:" + entry.ref);
      return label.isEmpty() ? entry.ref : label;
    }
  }
  return entry.ref;
}

void DownloadTracker::NoteTitle(const QString& source, const QString& ref, const QString& title) {
  titles_.insert(source + ":" + ref, title);
}

QString DownloadTracker::GameIdFor(const Entry& entry) {
  if (entry.kind == Kind::Game) return entry.ref;
  // Steam hands the install to its own client; the game is scanned in later.
  if (entry.kind == Kind::Title && entry.source != "steam") return entry.source + "-" + entry.ref;
  return QString();
}

void DownloadTracker::Poll() {
  bool any = false;
  for (const Entry& entry : entries_) {
    if (entry.kind != Kind::Game || entry.state != State::Running) continue;
    any = true;
    const QString key = entry.key;
    api::GetInstallProgressAsync(this, entry.ref.toStdString(), [this, key](InstallProgressResult progress) {
      auto found = std::find_if(entries_.begin(), entries_.end(), [&key](const Entry& e) { return e.key == key; });
      if (!progress.ok || found == entries_.end() || found->state != State::Running) return;
      found->bytes = progress.bytes_written;
      // In case the event was missed.
      if (progress.state == "finished") found->state = State::Finished;
      if (progress.state == "failed") found->state = State::Failed;
      emit Changed(key);
    });
  }
  if (!any) poll_->stop();
}

void DownloadTracker::ResolveNames(const QString& source) {
  if (asked_sources_.contains(source)) return;
  asked_sources_.insert(source);
  api::GetStoreLibraryAsync(this, source.toStdString(), false, [this, source](StoreLibraryResult result) {
    if (!result.ok) return;
    for (const StoreTitle& title : result.titles) {
      NoteTitle(source, QString::fromStdString(title.ref), QString::fromStdString(title.title));
    }
    emit Changed(QString());
  });
}

}  // namespace mira_gui
