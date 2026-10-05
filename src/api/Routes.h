#pragma once

#include <string>

#include "config/Config.h"

namespace httplib {
class Server;
struct Response;
}  // namespace httplib

namespace mira::api {

class Services;

// Each registers one area of the API on `http`; handlers keep a reference to `s`, which outlives the server.
void RegisterConfigRoutes(httplib::Server& http, Services& s);
void RegisterGameRoutes(httplib::Server& http, Services& s);
void RegisterLaunchRoutes(httplib::Server& http, Services& s);
void RegisterLibraryRoutes(httplib::Server& http, Services& s);
void RegisterStoreRoutes(httplib::Server& http, Services& s);
void RegisterLauncherRoutes(httplib::Server& http, Services& s);
void RegisterMetadataRoutes(httplib::Server& http, Services& s);
void RegisterRunnerRoutes(httplib::Server& http, Services& s);
void RegisterEventRoutes(httplib::Server& http, Services& s);

// Serves one cached art slot for `id`, or 404s.
void SendCachedArtwork(const config::Config& config, const std::string& id, const std::string& type,
                       httplib::Response& res);

}  // namespace mira::api
