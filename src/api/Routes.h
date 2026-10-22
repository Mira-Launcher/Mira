#pragma once

#include <functional>
#include <string>

#include "config/Config.h"
#include "store/MetadataStore.h"

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace mira::api {

class Services;

// Registers routes on `http` with each handler behind `gate`, which answers instead when it returns
// true. Unlike httplib's pre-routing handler, the gate sees the request body.
class Router {
 public:
  using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;
  using Gate = std::function<bool(const httplib::Request&, httplib::Response&)>;
  Router(httplib::Server& http, Gate gate) : http_(http), gate_(std::move(gate)) {}

  void Get(const std::string& pattern, Handler handler);
  void Post(const std::string& pattern, Handler handler);
  void Put(const std::string& pattern, Handler handler);
  void Patch(const std::string& pattern, Handler handler);
  void Delete(const std::string& pattern, Handler handler);

 private:
  Handler Gated(Handler handler) const;

  httplib::Server& http_;
  Gate gate_;
};

// Each registers one area of the API on `http`; handlers keep a reference to `s`, which outlives
// the server.
void RegisterConfigRoutes(Router& http, Services& s);
void RegisterGameRoutes(Router& http, Services& s);
void RegisterLaunchRoutes(Router& http, Services& s);
void RegisterLibraryRoutes(Router& http, Services& s);
void RegisterTagsRoutes(Router& http, Services& s);
void RegisterHiddenRoutes(Router& http, Services& s);
void RegisterStoreRoutes(Router& http, Services& s);
void RegisterLauncherRoutes(Router& http, Services& s);
void RegisterEmulatorRoutes(Router& http, Services& s);
void RegisterModManagerRoutes(Router& http, Services& s);
void RegisterUpdateRoutes(Router& http, Services& s);
void RegisterMetadataRoutes(Router& http, Services& s);
void RegisterAchievementRoutes(Router& http, Services& s);
void RegisterSaveRoutes(Router& http, Services& s);
void RegisterRunnerRoutes(Router& http, Services& s);
void RegisterEventRoutes(Router& http, Services& s);
void RegisterLogRoutes(Router& http, Services& s);

// Serves one cached art slot for `id`, or 404s.
void SendCachedArtwork(const store::MetadataStore& cache, const std::string& id,
                       const std::string& type, httplib::Response& res);

}  // namespace mira::api
