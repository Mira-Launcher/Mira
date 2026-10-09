#include "QuickSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include "../client/api/Games.h"

namespace mira_gui::bigscreen {
namespace {

// MANGOHUD_CONFIG entries other than the two this owns, kept as the user set them.
QStringList OtherEntries(const QString& config) {
  QStringList kept;
  for (const QString& entry : config.split(',', Qt::SkipEmptyParts)) {
    const QString key = entry.section('=', 0, 0).trimmed();
    if (key != "fps_limit" && key != "no_display") kept << entry.trimmed();
  }
  return kept;
}

}  // namespace

void LoadQuickSettings(QObject* context, const std::string& id, std::function<void(bool, QuickSettings)> done) {
  api::GetGameAsync(context, id, [context, id, done](GameDetailResult game) {
    if (!game.ok) return done(false, {});
    QuickSettings settings;
    const QJsonObject env = QJsonDocument::fromJson(QByteArray::fromStdString(game.game.env_json)).object();
    const bool mangohud = env.value("MANGOHUD").toString() == "1";
    const QString config = env.value("MANGOHUD_CONFIG").toString();
    bool hidden = false;
    for (const QString& entry : config.split(',', Qt::SkipEmptyParts)) {
      if (entry.trimmed() == "no_display") hidden = true;
      if (entry.section('=', 0, 0).trimmed() == "fps_limit") settings.fps_limit = entry.section('=', 1).trimmed().toInt();
    }
    if (!mangohud) settings.fps_limit = 0;
    // A MANGOHUD the game's env sets by hand shows it too.
    settings.overlay = mangohud && !hidden && settings.fps_limit == 0;
    api::GetGameConfigAsync(context, id, [settings, done](GameConfigResult config) mutable {
      for (const GameConfigEntry& entry : config.entries) {
        if (entry.key == "launch.gamemode") settings.gamemode = entry.value_display == "true";
        if (entry.key == "launch.mangohud" && entry.value_display == "true") settings.overlay = true;
      }
      done(true, settings);
    });
  });
}

void SaveQuickSettings(QObject* context, const std::string& id, const QuickSettings& settings,
                       std::function<void(QString)> done) {
  api::GetGameAsync(context, id, [context, id, settings, done](GameDetailResult game) {
    if (!game.ok) return done(QString::fromStdString(game.error.message));
    const QJsonObject env = QJsonDocument::fromJson(QByteArray::fromStdString(game.game.env_json)).object();
    QStringList config = OtherEntries(env.value("MANGOHUD_CONFIG").toString());
    if (settings.fps_limit > 0) config << QString("fps_limit=%1").arg(settings.fps_limit);
    if (settings.fps_limit > 0 && !settings.overlay) config << "no_display";
    // The overlay is launch.mangohud; the env loads MangoHud only for the FPS limit.
    const bool mangohud = settings.fps_limit > 0;
    // A null removes a key (docs/api.md, PATCH /v1/games/{id}).
    QJsonObject patch_env;
    patch_env["MANGOHUD"] = mangohud ? QJsonValue("1") : QJsonValue(QJsonValue::Null);
    patch_env["MANGOHUD_CONFIG"] = config.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(config.join(','));
    GamePatch patch;
    patch.env_json = QJsonDocument(patch_env).toJson(QJsonDocument::Compact).toStdString();
    api::PatchGameAsync(context, id, patch, [context, id, settings, done](PatchGameResult patched) {
      if (!patched.ok) return done(QString::fromStdString(patched.error.message));
      const std::vector<GameConfigEdit> edits = {
          {"launch.gamemode", "a boolean", settings.gamemode ? "true" : "false"},
          {"launch.mangohud", "a boolean", settings.overlay ? "true" : "false"}};
      api::PatchGameConfigAsync(context, id, edits, [done](PatchGameConfigResult result) {
        done(result.ok ? QString() : QString::fromStdString(result.error.message));
      });
    });
  });
}

}  // namespace mira_gui::bigscreen
